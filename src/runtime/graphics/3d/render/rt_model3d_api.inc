//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/render/rt_model3d_api.inc
// Purpose: Implement SceneAsset loading, persistence, accessors, lookup, and instantiation.
// Key invariants:
//   - Public accessors clamp corrupt count/capacity pairs and reject wrong-class entries.
//   - Instantiation clones mutable scene state while preserving retained resource ownership.
// Ownership/Lifetime:
//   - Load methods return owned SceneAsset handles; getters return borrowed runtime handles.
//   - Result wrappers retain successful values before releasing the loader-local reference.
// Links: rt_model3d.c, rt_model3d.h, rt_model3d_core.inc, rt_model3d_loaders.inc
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements Model3D public loading, access, persistence, LOD, and instantiation APIs.
/// @details This include fragment is compiled as part of `rt_model3d.c`. Owned load results are
///          separated from borrowed catalog accessors, and Result wrappers transfer loader-local
///          references into recoverable success/error values.

/// @brief Convert a SceneAsset load return value into a Result.
/// @details Successful values are retained by `Result.Ok` and then released from the local
///          loader reference. Failures use the thread-local asset diagnostic message captured by
///          the compatibility APIs, falling back to @p fallback when no message is available.
/// @param value Loader return value, or NULL on recoverable failure.
/// @param fallback Fallback error message for empty diagnostics.
/// @return Owned `Zanna.Result` carrying @p value or an error string.
static void *model3d_load_value_to_result(void *value, const char *fallback) {
    if (value) {
        void *result = rt_result_ok(value);
        model_release_local(value);
        return result;
    }

    const char *message = rt_asset_error_get_message();
    if (!message || message[0] == '\0') {
        rt_string err = rt_const_cstr(fallback ? fallback : "SceneAsset load failed");
        void *result = rt_result_err_str(err);
        rt_string_unref(err);
        return result;
    }

    rt_string err = rt_string_from_bytes(message, strlen(message));
    void *result = rt_result_err_str(err);
    rt_str_release_maybe(err);
    return result;
}

/// @brief Load a model from the filesystem (no asset-manager resolution). See header.
/// @param path Filesystem path with a supported extension.
/// @return New Model3D handle, or NULL after recording load diagnostics.
void *rt_model3d_load(rt_string path) {
    rt_asset_error_begin_load();
    void *model = rt_model3d_load_impl(path, 0, NULL, 0, NULL, NULL, 0, NULL, 0);
    if (model) {
        rt_asset_error_end_load_success();
    } else {
        rt_asset_error_set_if_empty(RT_ASSET_ERROR_CORRUPT, "Model3D.Load: failed to load model");
        rt_asset_error_end_load_failure();
    }
    return model;
}

/// @brief Load a model from the filesystem as `Result.Ok(SceneAsset)` or `Result.Err(String)`.
/// @param path Filesystem path to load.
/// @return Owned Zanna.Result value.
void *rt_model3d_load_result(rt_string path) {
    return model3d_load_value_to_result(rt_model3d_load(path), "SceneAsset.Load failed");
}

/// @brief Load VSCN text as a SceneAsset without reading the document from disk (ADR 0190).
/// @details @p path must carry a `.scene3d`/`.vscn` extension; it routes format detection,
///          diagnostics, and relative prefab base-directory resolution (referenced prefab
///          files still load from disk under the ADR 0187 guards). Traps on null arguments
///          like the sibling loaders; wrong extensions and malformed text are recoverable
///          failures reported through the shared asset-diagnostic channel.
/// @param path Logical VSCN path used for extension and dependency-base resolution.
/// @param text Complete VSCN source text.
/// @return New Model3D handle, or NULL after recording a recoverable diagnostic.
static void *model3d_load_text_value(rt_string path, rt_string text) {
    rt_asset_error_begin_load();
    if (!path || !rt_string_cstr(path)) {
        rt_asset_error_end_load_failure();
        rt_trap("SceneAsset.LoadTextResult: invalid path");
        return NULL;
    }
    if (!text) {
        rt_asset_error_end_load_failure();
        rt_trap("SceneAsset.LoadTextResult: text must not be null");
        return NULL;
    }
    void *model = rt_model3d_load_impl(
        path, 0, NULL, 0, NULL, NULL, 0, rt_string_cstr(text), (size_t)rt_str_len(text));
    if (model) {
        rt_asset_error_end_load_success();
    } else {
        rt_asset_error_set_if_empty(RT_ASSET_ERROR_CORRUPT,
                                    "SceneAsset.LoadTextResult: failed to load scene text");
        rt_asset_error_end_load_failure();
    }
    return model;
}

/// @brief Result companion for VSCN text loading: `Ok(SceneAsset)` or `Err(message)`.
/// @param path Logical VSCN path used for extension and dependency-base resolution.
/// @param text Complete VSCN source text.
/// @return Owned Result containing a Model3D or diagnostic string.
void *rt_model3d_load_text_result(rt_string path, rt_string text) {
    return model3d_load_value_to_result(model3d_load_text_value(path, text),
                                        "SceneAsset.LoadTextResult failed");
}

/// @brief Save the complete Model3D inventory through the model-aware VSCN v5 serializer.
/// @param obj Model3D whose scenes and retained resource catalogs are serialized.
/// @param path Destination filesystem path.
/// @return One after successful atomic publication; otherwise zero.
int64_t rt_model3d_save(void *obj, rt_string path) {
    rt_model3d *model = model3d_checked(obj);
    rt_vscn_asset_save_view view = {0};
    rt_vscn_asset_scene_view *scenes = NULL;
    int32_t scene_count;
    int64_t result;
    if (!model || !path || !rt_string_is_handle(path) || !rt_string_cstr(path)) {
        rt_asset_error_set_if_empty(RT_ASSET_ERROR_CORRUPT,
                                    "VSCN.Save: invalid model handle or destination path");
        return 0;
    }
    scene_count = model_scene_count(model);
    if (scene_count <= 0 || !model->template_root) {
        rt_asset_error_set_if_empty(RT_ASSET_ERROR_CORRUPT,
                                    "VSCN.Save: model has no serializable scene");
        return 0;
    }
    scenes = (rt_vscn_asset_scene_view *)calloc((size_t)scene_count, sizeof(*scenes));
    if (!scenes)
        return 0;
    for (int32_t i = 0; i < scene_count; ++i) {
        model3d_scene_entry *source = &model->scenes[i];
        scenes[i].root =
            (rt_scene_node3d *)rt_g3d_checked_or_null(source->root, RT_G3D_SCENENODE3D_CLASS_ID);
        scenes[i].name = source->name ? source->name : (i == 0 ? "default" : "scene");
        scenes[i].camera_count = model_clamped_array_count(
            source->cameras, source->camera_count, source->camera_capacity);
        scenes[i].cameras = source->cameras;
        if (!scenes[i].root) {
            free(scenes);
            return 0;
        }
    }
    view.meshes = model->meshes;
    view.mesh_count =
        model_clamped_array_count(model->meshes, model->mesh_count, model->mesh_capacity);
    view.materials = model->materials;
    view.material_count = model_clamped_array_count(
        model->materials, model->material_count, model->material_capacity);
    view.skeletons = model->skeletons;
    view.skeleton_count = model_clamped_array_count(
        model->skeletons, model->skeleton_count, model->skeleton_capacity);
    view.animations = model->animations;
    view.animation_count = model_clamped_array_count(
        model->animations, model->animation_count, model->animation_capacity);
    view.node_animations = model->node_animations;
    view.node_animation_count = model_clamped_array_count(
        model->node_animations, model->node_animation_count, model->node_animation_capacity);
    view.cameras = model->cameras;
    view.camera_count =
        model_clamped_array_count(model->cameras, model->camera_count, model->camera_capacity);
    view.scenes = scenes;
    view.scene_count = scene_count;
    view.variant_names = (const char *const *)model->variant_names;
    view.variant_count =
        model->variant_names && model->variant_count > 0 ? model->variant_count : 0;
    result = rt_vscn_save_asset_view(&view, path);
    free(scenes);
    return result;
}

/// @brief Validate one strictly ascending kept-index list against a clip array's safe count.
/// @param indices Candidate kept indices, or NULL when @p index_count is zero.
/// @param index_count Number of kept indices.
/// @param available Safe readable clip count.
/// @return Nonzero when every index is in range and the list is strictly ascending.
static int model_subset_indices_valid(const int64_t *indices,
                                      int32_t index_count,
                                      int32_t available) {
    if (index_count < 0 || index_count > available)
        return 0;
    if (index_count == 0)
        return 1;
    if (!indices)
        return 0;
    for (int32_t i = 0; i < index_count; i++) {
        if (indices[i] < 0 || indices[i] >= (int64_t)available)
            return 0;
        if (i > 0 && indices[i] <= indices[i - 1])
            return 0;
    }
    return 1;
}

/// @brief Compact one clip array to a validated kept-index list, releasing dropped clips.
/// @param arr Clip pointer array.
/// @param count In/out logical clip count.
/// @param capacity Allocated clip capacity (unchanged; the array is not shrunk).
/// @param indices Strictly ascending kept indices already validated by the caller.
/// @param index_count Number of kept indices.
/// @param class_id Runtime class released for dropped entries.
static void model_subset_apply(void **arr,
                               int32_t *count,
                               int32_t capacity,
                               const int64_t *indices,
                               int32_t index_count,
                               int64_t class_id) {
    int32_t safe = model_clamped_array_count(arr, *count, capacity);
    int32_t kept = 0;
    for (int32_t i = 0; i < safe; i++) {
        if (kept < index_count && indices[kept] == (int64_t)i) {
            arr[kept] = arr[i];
            kept++;
        } else {
            model_release_class_ref(&arr[i], class_id);
        }
    }
    for (int32_t i = kept; i < safe; i++)
        arr[i] = NULL;
    *count = kept;
}

int64_t rt_model3d_keep_animation_subset(void *obj,
                                         const int64_t *animation_indices,
                                         int32_t animation_index_count,
                                         const int64_t *node_animation_indices,
                                         int32_t node_animation_index_count) {
    rt_model3d *model = model3d_checked(obj);
    int32_t safe_animations;
    int32_t safe_node_animations;
    if (!model)
        return 0;
    safe_animations = model_clamped_array_count(
        model->animations, model->animation_count, model->animation_capacity);
    safe_node_animations = model_clamped_array_count(
        model->node_animations, model->node_animation_count, model->node_animation_capacity);
    if (!model_subset_indices_valid(animation_indices, animation_index_count, safe_animations) ||
        !model_subset_indices_valid(
            node_animation_indices, node_animation_index_count, safe_node_animations)) {
        return 0;
    }
    model_subset_apply(model->animations,
                       &model->animation_count,
                       model->animation_capacity,
                       animation_indices,
                       animation_index_count,
                       RT_G3D_ANIMATION3D_CLASS_ID);
    model_subset_apply(model->node_animations,
                       &model->node_animation_count,
                       model->node_animation_capacity,
                       node_animation_indices,
                       node_animation_index_count,
                       RT_G3D_NODEANIMATION3D_CLASS_ID);
    return 1;
}

/// @brief Duplicate-suppression table for model hierarchy preflight walks.
typedef struct model_node_seen_set {
    rt_scene_node3d **slots;
    size_t count;
    size_t capacity;
} model_node_seen_set;

/// @brief Mix an aligned node pointer for a power-of-two open-addressed table.
static size_t model_node_pointer_hash(const rt_scene_node3d *node) {
    uintptr_t bits = (uintptr_t)node;
    bits ^= bits >> 16;
    bits *= (uintptr_t)UINT32_C(0x7FEB352D);
    bits ^= bits >> 15;
    return (size_t)bits;
}

/// @brief Grow a model traversal's seen-node table without disturbing it on failure.
static int model_node_seen_reserve(model_node_seen_set *seen, size_t minimum_capacity) {
    size_t capacity;
    rt_scene_node3d **slots;
    if (!seen)
        return 0;
    capacity = seen->capacity > 0 ? seen->capacity : 64u;
    while (capacity < minimum_capacity) {
        if (capacity > SIZE_MAX / 2u)
            return 0;
        capacity *= 2u;
    }
    if (capacity == seen->capacity)
        return 1;
    if (capacity > SIZE_MAX / sizeof(*slots))
        return 0;
    slots = (rt_scene_node3d **)calloc(capacity, sizeof(*slots));
    if (!slots)
        return 0;
    for (size_t i = 0; i < seen->capacity; i++) {
        rt_scene_node3d *node = seen->slots[i];
        size_t index;
        if (!node)
            continue;
        index = model_node_pointer_hash(node) & (capacity - 1u);
        while (slots[index])
            index = (index + 1u) & (capacity - 1u);
        slots[index] = node;
    }
    free(seen->slots);
    seen->slots = slots;
    seen->capacity = capacity;
    return 1;
}

/// @brief Record one node once in a model traversal.
static int model_node_seen_insert(model_node_seen_set *seen, rt_scene_node3d *node, int *inserted) {
    size_t index;
    if (!seen || !node || !inserted)
        return 0;
    *inserted = 0;
    if (seen->capacity == 0 || seen->count >= seen->capacity / 2u) {
        size_t requested = seen->capacity > 0 ? seen->capacity * 2u : 64u;
        if (requested < seen->capacity || !model_node_seen_reserve(seen, requested))
            return 0;
    }
    index = model_node_pointer_hash(node) & (seen->capacity - 1u);
    while (seen->slots[index]) {
        if (seen->slots[index] == node)
            return 1;
        index = (index + 1u) & (seen->capacity - 1u);
    }
    seen->slots[index] = node;
    seen->count++;
    *inserted = 1;
    return 1;
}

/// @brief Append a node to a checked growable pointer vector.
static int model_node_vector_push(rt_scene_node3d ***nodes_io,
                                  size_t *count_io,
                                  size_t *capacity_io,
                                  rt_scene_node3d *node) {
    rt_scene_node3d **nodes;
    size_t count;
    size_t capacity;
    if (!nodes_io || !count_io || !capacity_io || !node)
        return 0;
    nodes = *nodes_io;
    count = *count_io;
    capacity = *capacity_io;
    if (count >= capacity) {
        size_t new_capacity = capacity > 0 ? capacity * 2u : 64u;
        rt_scene_node3d **grown;
        if (new_capacity <= capacity || new_capacity > SIZE_MAX / sizeof(*grown))
            return 0;
        grown = (rt_scene_node3d **)realloc(nodes, new_capacity * sizeof(*grown));
        if (!grown)
            return 0;
        nodes = grown;
        capacity = new_capacity;
    }
    nodes[count++] = node;
    *nodes_io = nodes;
    *count_io = count;
    *capacity_io = capacity;
    return 1;
}

/// @brief Collect every live template/scene node exactly once before mutation.
/// @details The breadth-first vector doubles as the work queue. Root counts are not capped by a
/// fixed local array, shared subtrees and cycles terminate through the seen set, and callers never
/// begin an apply pass unless the complete walk has succeeded.
/// @param model Validated model whose template and scene roots are traversed.
/// @param[out] nodes_out Owned node vector on success; `NULL` on failure.
/// @param[out] count_out Number of initialized entries in @p nodes_out.
/// @param trap_message Allocation/limit diagnostic emitted before failure.
/// @return Nonzero on complete success, otherwise zero.
static int model_collect_nodes(rt_model3d *model,
                               rt_scene_node3d ***nodes_out,
                               size_t *count_out,
                               const char *trap_message) {
    rt_scene_node3d **nodes = NULL;
    size_t count = 0;
    size_t capacity = 0;
    model_node_seen_set seen;
    int inserted;
    if (nodes_out)
        *nodes_out = NULL;
    if (count_out)
        *count_out = 0;
    if (!model || !nodes_out || !count_out)
        return 0;
    memset(&seen, 0, sizeof(seen));

#define MODEL_COLLECT_ROOT(candidate_)                                                             \
    do {                                                                                           \
        rt_scene_node3d *root_ =                                                                   \
            (rt_scene_node3d *)rt_g3d_checked_or_null((candidate_), RT_G3D_SCENENODE3D_CLASS_ID);  \
        if (root_) {                                                                               \
            if (!model_node_seen_insert(&seen, root_, &inserted) ||                                \
                (inserted && !model_node_vector_push(&nodes, &count, &capacity, root_)))           \
                goto fail;                                                                         \
        }                                                                                          \
    } while (0)

    MODEL_COLLECT_ROOT(model->template_root);
    for (int32_t si = 0, scene_count = model_scene_count(model); si < scene_count; si++)
        MODEL_COLLECT_ROOT(model->scenes[si].root);
#undef MODEL_COLLECT_ROOT

    for (size_t read = 0; read < count; read++) {
        rt_scene_node3d *current = nodes[read];
        int32_t child_count;
        if (count > (size_t)MODEL3D_MAX_WALK_NODES)
            goto fail;
        child_count = model_node_child_count(current);
        for (int32_t i = 0; i < child_count; i++) {
            rt_scene_node3d *child = (rt_scene_node3d *)rt_g3d_checked_or_null(
                current->children[i], RT_G3D_SCENENODE3D_CLASS_ID);
            if (!child)
                continue;
            if (!model_node_seen_insert(&seen, child, &inserted) ||
                (inserted && !model_node_vector_push(&nodes, &count, &capacity, child)))
                goto fail;
        }
    }
    free(seen.slots);
    *nodes_out = nodes;
    *count_out = count;
    return 1;

fail:
    free(seen.slots);
    free(nodes);
    rt_trap(trap_message);
    return 0;
}

/// @brief model_visit_nodes callback: detach the node's mesh and material.
static void model_strip_node_visuals(rt_scene_node3d *node, void *ctx) {
    (void)ctx;
    rt_scene_node3d_set_mesh(node, NULL);
    rt_scene_node3d_set_material(node, NULL);
}

/// @brief Drop every mesh and material from the asset (animation-only bake).
/// @details Tool-facing (`zanna asset bake --strip-meshes`): clip bakes only
///          need the animations and skeletons, but the importer carries the
///          full render mesh into every per-clip document — hundreds of MB
///          of dead payload across a clip library. Nodes keep their
///          transforms/names so skeleton hierarchies survive; loaders accept
///          mesh-less documents.
/// @param obj Model3D to strip in place.
/// @return Number of meshes dropped, or 0 for invalid input.
int64_t rt_model3d_strip_meshes(void *obj) {
    rt_model3d *model = model3d_checked(obj);
    rt_scene_node3d **nodes = NULL;
    size_t node_count = 0;
    int32_t safe_meshes;
    int32_t safe_materials;
    if (!model)
        return 0;
    if (!model_collect_nodes(
            model, &nodes, &node_count, "SceneAsset.StripMeshes: node walk allocation failed"))
        return 0;
    for (size_t i = 0; i < node_count; i++)
        model_strip_node_visuals(nodes[i], NULL);
    free(nodes);
    safe_meshes = model_clamped_array_count(model->meshes, model->mesh_count, model->mesh_capacity);
    for (int32_t i = 0; i < safe_meshes; i++) {
        model_release_local(model->meshes[i]);
        model->meshes[i] = NULL;
    }
    model->mesh_count = 0;
    safe_materials = model_clamped_array_count(
        model->materials, model->material_count, model->material_capacity);
    for (int32_t i = 0; i < safe_materials; i++) {
        model_release_local(model->materials[i]);
        model->materials[i] = NULL;
    }
    model->material_count = 0;
    return safe_meshes;
}

/// @brief Context for the simplify-remap node visit.
typedef struct {
    void *old_mesh;
    void *new_mesh;
} model_mesh_swap_ctx;

/// @brief model_visit_nodes callback: retarget nodes from old_mesh to new_mesh.
static void model_swap_node_mesh(rt_scene_node3d *node, void *ctx) {
    model_mesh_swap_ctx *swap = (model_mesh_swap_ctx *)ctx;
    if (node->mesh == swap->old_mesh)
        rt_scene_node3d_set_mesh(node, swap->new_mesh);
}

/// @brief Decimate every mesh above @p max_tris in place (QEM subset placement).
/// @details Tool-facing (`zanna asset bake --simplify-meshes N`): heavy prop
///          scans (a 1.9M-triangle glove) can be shipped pre-decimated so no
///          consumer pays the QEM cost at load. Node references are remapped
///          to the decimated meshes.
/// @param obj Model3D to decimate in place.
/// @param max_tris Triangle ceiling per mesh (minimum 8).
/// @return Number of meshes replaced, or 0 for invalid input.
int64_t rt_model3d_simplify_meshes(void *obj, int64_t max_tris) {
    return rt_model3d_simplify_meshes_ex(obj, max_tris, 0, 0.0);
}

/// @brief Decimate every mesh above @p max_tris with explicit collapse constraints.
/// @details Tool-facing peer of rt_model3d_simplify_meshes: @p flags and
///          @p max_error_frac flow to rt_mesh3d_simplify_ex per mesh, so bakes
///          of seam-dense photogrammetry can lock boundary polylines and stop
///          at a geometric error ceiling instead of shattering to the budget.
/// @param obj Model3D to decimate in place.
/// @param max_tris Triangle ceiling per mesh (minimum 8).
/// @param flags Bitwise OR of RT_MESH3D_SIMPLIFY_FLAG_* values, or zero.
/// @param max_error_frac Error ceiling as a bounding-diameter fraction; <= 0 disables.
/// @return Number of meshes replaced, or 0 for invalid input.
int64_t rt_model3d_simplify_meshes_ex(void *obj,
                                      int64_t max_tris,
                                      int64_t flags,
                                      double max_error_frac) {
    rt_model3d *model = model3d_checked(obj);
    rt_scene_node3d **nodes = NULL;
    size_t node_count = 0;
    int32_t safe_meshes;
    int64_t replaced = 0;
    if (!model)
        return 0;
    if (max_tris < 8)
        max_tris = 8;
    if (!model_collect_nodes(
            model, &nodes, &node_count, "SceneAsset.SimplifyMeshes: node walk allocation failed"))
        return 0;
    safe_meshes = model_clamped_array_count(model->meshes, model->mesh_count, model->mesh_capacity);
    for (int32_t i = 0; i < safe_meshes; i++) {
        void *mesh_obj = model->meshes[i];
        rt_mesh3d *mesh = (rt_mesh3d *)rt_g3d_checked_or_null(mesh_obj, RT_G3D_MESH3D_CLASS_ID);
        void *slim;
        model_mesh_swap_ctx swap;
        if (!mesh)
            continue;
        rt_mesh3d_repair_geometry_counts(mesh);
        if ((int64_t)(rt_mesh3d_validated_index_count(mesh) / 3u) <= max_tris)
            continue;
        slim = rt_mesh3d_simplify_ex(mesh_obj, max_tris, flags, max_error_frac);
        if (!slim)
            continue;
        swap.old_mesh = mesh_obj;
        swap.new_mesh = slim;
        for (size_t node_index = 0; node_index < node_count; node_index++)
            model_swap_node_mesh(nodes[node_index], &swap);
        model->meshes[i] = slim; /* keep the construction ref as the array ref */
        model_release_local(mesh_obj);
        replaced++;
    }
    free(nodes);
    return replaced;
}

/// @brief One texture-slot rewrite step for rt_model3d_limit_texture_dim.
/// @details Resolves the slot's texture reference to decoded pixels; when
///   either dimension exceeds @p max_dim, produces a proportionally
///   downscaled Pixels and assigns it through @p setter (which owns the
///   retain/release of the material slot). A tiny caller-owned dedup table
///   maps already-processed source refs to their downscaled result so a
///   texture shared by several slots/materials is resized exactly once.
/// @param material_obj Material3D handle owning the slot.
/// @param slot_ref Borrowed current slot reference (Pixels/TextureAsset3D).
/// @param setter Slot assignment entry point (e.g. rt_material3d_set_texture).
/// @param max_dim Dimension ceiling in texels.
/// @param seen_src Parallel dedup arrays (source refs).
/// @param seen_dst Parallel dedup arrays (downscaled results).
/// @param seen_count In/out entry count for the dedup arrays.
/// @param seen_cap Capacity of the dedup arrays.
/// @return 1 when the slot was rewritten, 0 otherwise.
static int model_limit_texture_slot(void *material_obj,
                                    void *slot_ref,
                                    void (*setter)(void *, void *),
                                    int64_t max_dim,
                                    void **seen_src,
                                    void **seen_dst,
                                    size_t *seen_count,
                                    size_t seen_cap) {
    void *pixels;
    void *resized;
    int64_t w;
    int64_t h;
    int64_t nw;
    int64_t nh;
    if (!material_obj || !slot_ref || !setter)
        return 0;
    for (size_t i = 0; i < *seen_count; i++) {
        if (seen_src[i] == slot_ref) {
            if (!seen_dst[i])
                return 0; /* previously inspected, already small enough */
            setter(material_obj, seen_dst[i]);
            return 1;
        }
    }
    pixels = rt_material3d_resolve_texture_pixels(slot_ref);
    if (!pixels)
        return 0;
    w = rt_pixels_width(pixels);
    h = rt_pixels_height(pixels);
    if (w <= 0 || h <= 0)
        return 0;
    if (w <= max_dim && h <= max_dim) {
        if (*seen_count < seen_cap) {
            seen_src[*seen_count] = slot_ref;
            seen_dst[*seen_count] = NULL;
            (*seen_count)++;
        }
        return 0;
    }
    if (w >= h) {
        nw = max_dim;
        nh = h * max_dim / w;
    } else {
        nh = max_dim;
        nw = w * max_dim / h;
    }
    if (nw < 1)
        nw = 1;
    if (nh < 1)
        nh = 1;
    resized = rt_pixels_resize(pixels, nw, nh);
    if (!resized)
        return 0;
    if (*seen_count < seen_cap) {
        seen_src[*seen_count] = slot_ref;
        seen_dst[*seen_count] = resized;
        (*seen_count)++;
    }
    setter(material_obj, resized);
    return 1;
}

/// @brief Downscale every material texture above @p max_dim in place.
/// @details Tool-facing (`zanna asset bake --max-texture-dim N`): imported
///   containers keep their ORIGINAL encoded bytes (VSCN's source-container
///   preservation), so an 8192-squared JPEG costs its full decode at every
///   load. Rewriting oversized slots to downscaled raw Pixels makes the
///   subsequent save serialize compact pixel payloads instead — the decode
///   and the bulk both disappear from the runtime path. Shared textures are
///   resized once; slots already within the ceiling are untouched.
/// @param obj Model3D whose materials are rewritten in place.
/// @param max_dim Texture dimension ceiling in texels (minimum 64).
/// @return Number of texture slots rewritten, or 0 for invalid input.
int64_t rt_model3d_limit_texture_dim(void *obj, int64_t max_dim) {
    rt_model3d *model = model3d_checked(obj);
    int32_t safe_materials;
    int64_t rewritten = 0;
    void *seen_src[64];
    void *seen_dst[64];
    size_t seen_count = 0;
    if (!model)
        return 0;
    if (max_dim < 64)
        max_dim = 64;
    safe_materials = model_clamped_array_count(
        model->materials, model->material_count, model->material_capacity);
    for (int32_t i = 0; i < safe_materials; i++) {
        rt_material3d *material = (rt_material3d *)rt_g3d_checked_or_null(
            model->materials[i], RT_G3D_MATERIAL3D_CLASS_ID);
        if (!material)
            continue;
        rewritten += model_limit_texture_slot(model->materials[i],
                                              material->texture,
                                              rt_material3d_set_texture,
                                              max_dim,
                                              seen_src,
                                              seen_dst,
                                              &seen_count,
                                              64);
        rewritten += model_limit_texture_slot(model->materials[i],
                                              material->normal_map,
                                              rt_material3d_set_normal_map,
                                              max_dim,
                                              seen_src,
                                              seen_dst,
                                              &seen_count,
                                              64);
        rewritten += model_limit_texture_slot(model->materials[i],
                                              material->specular_map,
                                              rt_material3d_set_specular_map,
                                              max_dim,
                                              seen_src,
                                              seen_dst,
                                              &seen_count,
                                              64);
        rewritten += model_limit_texture_slot(model->materials[i],
                                              material->emissive_map,
                                              rt_material3d_set_emissive_map,
                                              max_dim,
                                              seen_src,
                                              seen_dst,
                                              &seen_count,
                                              64);
        rewritten += model_limit_texture_slot(model->materials[i],
                                              material->metallic_roughness_map,
                                              rt_material3d_set_metallic_roughness_map,
                                              max_dim,
                                              seen_src,
                                              seen_dst,
                                              &seen_count,
                                              64);
        rewritten += model_limit_texture_slot(model->materials[i],
                                              material->ao_map,
                                              rt_material3d_set_ao_map,
                                              max_dim,
                                              seen_src,
                                              seen_dst,
                                              &seen_count,
                                              64);
        rewritten += model_limit_texture_slot(model->materials[i],
                                              material->lightmap,
                                              rt_material3d_set_lightmap,
                                              max_dim,
                                              seen_src,
                                              seen_dst,
                                              &seen_count,
                                              64);
    }
    return rewritten;
}

/// @brief Load a model with explicit import options (Plan 09).
/// @details `force_tangents` generates tangents for every UV0-mapped glTF primitive even
///   when its material has no normal map at load time — for assets whose materials gain
///   normal maps after import. FBX ignores the option. Options scope thread-locally
///   around the load, so the default entry points are unchanged.
/// @param path Filesystem model path.
/// @param force_tangents Nonzero to force eligible glTF tangent generation.
/// @return New Model3D handle, or NULL on failure.
void *rt_model3d_load_with_options(rt_string path, int8_t force_tangents) {
    rt_gltf_load_options opts = rt_gltf_load_options_default();
    rt_gltf_load_options saved;
    void *model;

    opts.force_tangents = force_tangents ? 1 : 0;
    saved = rt_gltf_set_thread_load_options(&opts);
    model = rt_model3d_load(path);
    (void)rt_gltf_set_thread_load_options(&saved);
    return model;
}

/// @brief Extensible options-string variant of `SceneAsset.Load`.
/// @details @p options is a comma/space-separated flag list. Recognized flags:
///          "forceTangents" (tangents for every UV0 primitive), "eightInfluences"
///          (keep up to 8 bone influences per vertex; influences 5-8 ride a mesh
///          side stream applied by CPU skinning), "compressAnimations"
///          (tolerance-based keyframe reduction on imported clips),
///          "compactStreams" (every imported mesh opts into the compact 48-byte
///          GPU static-cache vertex encoding). Unknown flags are ignored so
///          future options stay backward-compatible.
/// @param path Filesystem model path.
/// @param options Nullable comma/space-separated option string.
/// @return New Model3D handle, or NULL on failure.
void *rt_model3d_load_with_options_ex(rt_string path, rt_string options) {
    rt_gltf_load_options opts = rt_gltf_load_options_default();
    rt_gltf_load_options saved;
    void *model;
    int compact_streams = 0;
    const char *text = rt_string_cstr(options);
    if (text) {
        if (strstr(text, "forceTangents"))
            opts.force_tangents = 1;
        if (strstr(text, "eightInfluences"))
            opts.eight_bone_influences = 1;
        if (strstr(text, "compressAnimations"))
            opts.compress_animations = 1;
        if (strstr(text, "compactStreams"))
            compact_streams = 1;
    }
    saved = rt_gltf_set_thread_load_options(&opts);
    model = rt_model3d_load(path);
    (void)rt_gltf_set_thread_load_options(&saved);
    if (model && compact_streams) {
        int64_t mesh_count = rt_model3d_get_mesh_count(model);
        for (int64_t i = 0; i < mesh_count; i++)
            rt_mesh3d_set_compact_streams(rt_model3d_get_mesh(model, i), 1);
    }
    return model;
}

/// @brief Options-aware Result variant of `SceneAsset.Load` (Plan 09).
/// @param path Filesystem model path.
/// @param force_tangents Nonzero to force eligible glTF tangent generation.
/// @return Owned Result containing a Model3D or diagnostic string.
void *rt_model3d_load_result_with_options(rt_string path, int8_t force_tangents) {
    return model3d_load_value_to_result(rt_model3d_load_with_options(path, force_tangents),
                                        "SceneAsset.LoadWithOptions failed");
}

/// @brief Load a model through the asset manager (mounted/embedded + dev fallback). See header.
/// @param path Logical asset path or URI.
/// @return New Model3D handle, or NULL after recording load diagnostics.
void *rt_model3d_load_asset(rt_string path) {
    rt_asset_error_begin_load();
    void *model = rt_model3d_load_impl(path, 1, NULL, 0, NULL, NULL, 0, NULL, 0);
    if (model) {
        rt_asset_error_end_load_success();
    } else {
        rt_asset_error_set_if_empty(RT_ASSET_ERROR_CORRUPT,
                                    "Model3D.LoadAsset: failed to load model");
        rt_asset_error_end_load_failure();
    }
    return model;
}

/// @brief Load a model through the asset manager as `Result.Ok(SceneAsset)` or `Result.Err`.
/// @param path Asset path or URI to load.
/// @return Owned Zanna.Result value.
void *rt_model3d_load_asset_result(rt_string path) {
    return model3d_load_value_to_result(rt_model3d_load_asset(path), "SceneAsset.LoadAsset failed");
}

/// @brief Internal async path: build a glTF/GLB Model3D from worker-staged root bytes.
/// @details Takes ownership of @p preloaded_data. Non-glTF paths free it and use the normal
///   loader so conservative async callers do not leak when falling back to main-thread loading.
/// @param path Logical source path used for format and dependency resolution.
/// @param preloaded_data Owned root-file bytes, consumed on every path.
/// @param preloaded_size Byte count of @p preloaded_data.
/// @param load_assets Nonzero to resolve dependencies through the asset manager.
/// @return New Model3D handle, or NULL on failure.
void *rt_model3d_load_preloaded_gltf(rt_string path,
                                     uint8_t *preloaded_data,
                                     size_t preloaded_size,
                                     int load_assets) {
    const char *path_cstr = path ? rt_string_cstr(path) : NULL;
    if (!path_cstr || !(model_has_ext(path_cstr, ".gltf") || model_has_ext(path_cstr, ".glb"))) {
        free(preloaded_data);
        return load_assets ? rt_model3d_load_asset(path) : rt_model3d_load(path);
    }
    return rt_model3d_load_impl(
        path, load_assets ? 1 : 0, preloaded_data, preloaded_size, NULL, NULL, 0, NULL, 0);
}

/// @brief Internal async path: build an FBX Model3D from worker-staged root bytes.
/// @details Takes ownership of @p preloaded_data. Non-FBX paths free it and use the normal
///   loader so conservative async callers do not leak when falling back to main-thread loading.
/// @param path Logical source path used for format context and diagnostics.
/// @param preloaded_data Owned root-file bytes, consumed on every path.
/// @param preloaded_size Byte count of @p preloaded_data.
/// @param load_assets Nonzero to use asset-manager loading behavior.
/// @return New Model3D handle, or NULL on failure.
/// @brief Build a Model3D from VSCN/scene3d document bytes that were read off-thread.
/// @details Parses the owned byte buffer through the ADR 0190 text path (no file
///          IO on this thread); a non-VSCN extension falls back to a normal load.
///          The buffer is always freed by this call.
/// @param path Logical .scene3d/.vscn source path (prefab base + diagnostics).
/// @param preloaded_text Owned document bytes, consumed by this call.
/// @param preloaded_len Number of bytes in @p preloaded_text.
/// @param load_assets Nonzero to retain asset-manager dependency behavior.
/// @return Owned Model3D on success, or NULL on recoverable failure.
void *rt_model3d_load_preloaded_vscn(rt_string path,
                                     char *preloaded_text,
                                     size_t preloaded_len,
                                     int load_assets) {
    const char *path_cstr = path ? rt_string_cstr(path) : NULL;
    if (!path_cstr ||
        !(model_has_ext(path_cstr, ".scene3d") || model_has_ext(path_cstr, ".vscn")) ||
        !preloaded_text || preloaded_len == 0u) {
        free(preloaded_text);
        return load_assets ? rt_model3d_load_asset(path) : rt_model3d_load(path);
    }
    {
        void *model = rt_model3d_load_impl(
            path, load_assets ? 1 : 0, NULL, 0, NULL, NULL, 0, preloaded_text, preloaded_len);
        free(preloaded_text);
        return model;
    }
}

void *rt_model3d_load_preloaded_fbx(rt_string path,
                                    uint8_t *preloaded_data,
                                    size_t preloaded_size,
                                    int load_assets) {
    const char *path_cstr = path ? rt_string_cstr(path) : NULL;
    if (!path_cstr || !model_has_ext(path_cstr, ".fbx")) {
        free(preloaded_data);
        return load_assets ? rt_model3d_load_asset(path) : rt_model3d_load(path);
    }
    return rt_model3d_load_impl(
        path, load_assets ? 1 : 0, NULL, 0, NULL, preloaded_data, preloaded_size, NULL, 0);
}

/// @brief Build a Model3D from a glTF asset that was staged off-thread into @p bundle.
/// @details Loads the glTF on the main thread from the preload bundle (no file I/O), then wraps the
///          resulting asset's meshes/materials/scenes as a Model3D. Falls back to a normal load
///          when
///          @p bundle is NULL.
/// @param path Logical glTF/GLB source path.
/// @param bundle Owned staged root/dependency bundle, consumed by this call.
/// @param load_assets Nonzero to retain asset-manager dependency behavior.
/// @return New Model3D handle, or NULL on failure.
void *rt_model3d_load_preloaded_gltf_bundle(rt_string path,
                                            struct rt_gltf_preload_bundle *bundle,
                                            int load_assets) {
    const char *path_cstr = path ? rt_string_cstr(path) : NULL;
    if (!path_cstr || !(model_has_ext(path_cstr, ".gltf") || model_has_ext(path_cstr, ".glb"))) {
        rt_gltf_preload_bundle_free(bundle);
        return load_assets ? rt_model3d_load_asset(path) : rt_model3d_load(path);
    }
    return rt_model3d_load_impl(path, load_assets ? 1 : 0, NULL, 0, bundle, NULL, 0, NULL, 0);
}

/// @brief Number of meshes loaded into this model.
/// @param obj Model3D receiver.
/// @return Safe enumerable mesh count, or zero for invalid input.
int64_t rt_model3d_get_mesh_count(void *obj) {
    rt_model3d *model = model3d_checked(obj);
    return model ? model_clamped_array_count(model->meshes, model->mesh_count, model->mesh_capacity)
                 : 0;
}

/// @brief Number of materials loaded into this model.
/// @param obj Model3D receiver.
/// @return Safe enumerable material count, or zero for invalid input.
int64_t rt_model3d_get_material_count(void *obj) {
    rt_model3d *model = model3d_checked(obj);
    return model ? model_clamped_array_count(
                       model->materials, model->material_count, model->material_capacity)
                 : 0;
}

/// @brief Number of skeletons (bone hierarchies) loaded into this model.
/// @param obj Model3D receiver.
/// @return Safe enumerable skeleton count, or zero for invalid input.
int64_t rt_model3d_get_skeleton_count(void *obj) {
    rt_model3d *model = model3d_checked(obj);
    return model ? model_clamped_array_count(
                       model->skeletons, model->skeleton_count, model->skeleton_capacity)
                 : 0;
}

/// @brief Number of animation clips loaded into this model.
/// @param obj Model3D receiver.
/// @return Safe skeletal-animation count, or zero for invalid input.
int64_t rt_model3d_get_animation_count(void *obj) {
    rt_model3d *model = model3d_checked(obj);
    return model ? model_clamped_array_count(
                       model->animations, model->animation_count, model->animation_capacity)
                 : 0;
}

/// @brief Number of node/object/morph/camera animation clips loaded into this model.
/// @param obj Model3D receiver.
/// @return Safe node-animation count, or zero for invalid input.
int64_t rt_model3d_get_node_animation_count(void *obj) {
    rt_model3d *model = model3d_checked(obj);
    return model ? model_clamped_array_count(model->node_animations,
                                             model->node_animation_count,
                                             model->node_animation_capacity)
                 : 0;
}

/// @brief Number of model mesh slots carrying a valid attached MorphTarget3D.
/// @param obj Model3D receiver.
/// @return Count of valid mesh-attached morph-target containers.
int64_t rt_model3d_get_morph_target_count(void *obj) {
    rt_model3d *model = model3d_checked(obj);
    int32_t mesh_count =
        model ? model_clamped_array_count(model->meshes, model->mesh_count, model->mesh_capacity)
              : 0;
    int64_t count = 0;
    for (int32_t i = 0; i < mesh_count; ++i) {
        rt_mesh3d *mesh =
            (rt_mesh3d *)rt_g3d_checked_or_null(model->meshes[i], RT_G3D_MESH3D_CLASS_ID);
        if (mesh && rt_g3d_has_class(mesh->morph_targets_ref, RT_G3D_MORPHTARGET3D_CLASS_ID))
            count++;
    }
    return count;
}

/// @brief Total safe morph-shape count across every enumerable mesh.
/// @param obj Model3D receiver.
/// @return Total shape count saturated at `INT64_MAX`, or zero for invalid input.
int64_t rt_model3d_get_morph_shape_count(void *obj) {
    rt_model3d *model = model3d_checked(obj);
    int32_t mesh_count =
        model ? model_clamped_array_count(model->meshes, model->mesh_count, model->mesh_capacity)
              : 0;
    int64_t count = 0;
    for (int32_t i = 0; i < mesh_count; ++i) {
        rt_mesh3d *mesh =
            (rt_mesh3d *)rt_g3d_checked_or_null(model->meshes[i], RT_G3D_MESH3D_CLASS_ID);
        int64_t shapes = mesh ? rt_morphtarget3d_get_shape_count(mesh->morph_targets_ref) : 0;
        if (shapes > INT64_MAX - count)
            return INT64_MAX;
        if (shapes > 0)
            count += shapes;
    }
    return count;
}

/// @brief Number of scene nodes in the template subtree (excludes the synthetic root).
/// @param obj Model3D receiver.
/// @return Template node count excluding the synthetic root, or zero.
int64_t rt_model3d_get_node_count(void *obj) {
    rt_model3d *model = model3d_checked(obj);
    if (!model || !model->template_root)
        return 0;
    {
        int32_t count = model_count_subtree(model->template_root);
        return count > 0 ? (int64_t)(count - 1) : 0;
    }
}

/// @brief Number of immutable scenes addressable by this Model3D.
/// @details glTF imports order scenes with the active/default scene at index 0 and any secondary
///   scene roots after it. Other importers expose one default scene.
/// @param obj Model3D receiver.
/// @return Addressable immutable-scene count, or zero for invalid input.
int64_t rt_model3d_get_scene_count(void *obj) {
    rt_model3d *model = model3d_checked(obj);
    return model_scene_count(model);
}

/// @brief Number of cameras imported for @p scene_index.
/// @details glTF cameras are retained per immutable scene. Invalid scene indices and importers
///   without authored cameras return zero.
/// @param obj Model3D receiver.
/// @param scene_index Zero-based immutable-scene index.
/// @return Safe scene camera count, or zero for invalid input.
int64_t rt_model3d_get_camera_count(void *obj, int64_t scene_index) {
    rt_model3d *model = model3d_checked(obj);
    int32_t scene_count = model_scene_count(model);
    if (!model || scene_index < 0 || scene_index >= scene_count)
        return 0;
    return model_clamped_array_count(model->scenes[scene_index].cameras,
                                     model->scenes[scene_index].camera_count,
                                     model->scenes[scene_index].camera_capacity);
}

/// @brief Borrow the i-th Mesh3D (NULL on out-of-range). Caller must NOT release; the model owns
/// it.
/// @param obj Model3D receiver.
/// @param index Zero-based mesh index.
/// @return Borrowed valid Mesh3D, or NULL.
void *rt_model3d_get_mesh(void *obj, int64_t index) {
    rt_model3d *model = model3d_checked(obj);
    int32_t mesh_count =
        model ? model_clamped_array_count(model->meshes, model->mesh_count, model->mesh_capacity)
              : 0;
    if (!model || index < 0 || index >= mesh_count)
        return NULL;
    return rt_g3d_checked_or_null(model->meshes[index], RT_G3D_MESH3D_CLASS_ID);
}

/// @brief Borrow the i-th Material3D (NULL on out-of-range).
/// @param obj Model3D receiver.
/// @param index Zero-based material index.
/// @return Borrowed valid Material3D, or NULL.
void *rt_model3d_get_material(void *obj, int64_t index) {
    rt_model3d *model = model3d_checked(obj);
    int32_t material_count = model ? model_clamped_array_count(model->materials,
                                                               model->material_count,
                                                               model->material_capacity)
                                   : 0;
    if (!model || index < 0 || index >= material_count)
        return NULL;
    return rt_g3d_checked_or_null(model->materials[index], RT_G3D_MATERIAL3D_CLASS_ID);
}

/// @brief Borrow the i-th Skeleton3D (NULL on out-of-range).
/// @param obj Model3D receiver.
/// @param index Zero-based skeleton index.
/// @return Borrowed valid Skeleton3D, or NULL.
void *rt_model3d_get_skeleton(void *obj, int64_t index) {
    rt_model3d *model = model3d_checked(obj);
    int32_t skeleton_count = model ? model_clamped_array_count(model->skeletons,
                                                               model->skeleton_count,
                                                               model->skeleton_capacity)
                                   : 0;
    if (!model || index < 0 || index >= skeleton_count)
        return NULL;
    return rt_g3d_checked_or_null(model->skeletons[index], RT_G3D_SKELETON3D_CLASS_ID);
}

/// @brief Borrow the i-th Animation3D clip (NULL on out-of-range).
/// @param obj Model3D receiver.
/// @param index Zero-based skeletal-animation index.
/// @return Borrowed valid Animation3D, or NULL.
void *rt_model3d_get_animation(void *obj, int64_t index) {
    rt_model3d *model = model3d_checked(obj);
    int32_t animation_count = model ? model_clamped_array_count(model->animations,
                                                                model->animation_count,
                                                                model->animation_capacity)
                                    : 0;
    if (!model || index < 0 || index >= animation_count)
        return NULL;
    return rt_g3d_checked_or_null(model->animations[index], RT_G3D_ANIMATION3D_CLASS_ID);
}

/// @brief Borrow the i-th NodeAnimation3D clip (NULL on out-of-range).
/// @param obj Model3D receiver.
/// @param index Zero-based node-animation index.
/// @return Borrowed valid NodeAnimation3D, or NULL.
void *rt_model3d_get_node_animation(void *obj, int64_t index) {
    rt_model3d *model = model3d_checked(obj);
    int32_t animation_count = model ? model_clamped_array_count(model->node_animations,
                                                                model->node_animation_count,
                                                                model->node_animation_capacity)
                                    : 0;
    if (!model || index < 0 || index >= animation_count)
        return NULL;
    return rt_g3d_checked_or_null(model->node_animations[index], RT_G3D_NODEANIMATION3D_CLASS_ID);
}

/// @brief Borrow the MorphTarget3D attached to mesh @p mesh_index.
/// @param obj Model3D receiver.
/// @param mesh_index Zero-based enumerable mesh index.
/// @return Borrowed valid MorphTarget3D, or NULL.
void *rt_model3d_get_morph_target(void *obj, int64_t mesh_index) {
    rt_mesh3d *mesh = (rt_mesh3d *)rt_model3d_get_mesh(obj, mesh_index);
    return mesh ? rt_g3d_checked_or_null(mesh->morph_targets_ref, RT_G3D_MORPHTARGET3D_CLASS_ID)
                : NULL;
}

/// @brief Borrow the i-th NodeAnimation3D name, or an empty string on out-of-range.
/// @param obj Model3D receiver.
/// @param index Zero-based node-animation index.
/// @return Borrowed clip name, or an empty runtime string.
rt_string rt_model3d_get_node_animation_name(void *obj, int64_t index) {
    return rt_node_animation3d_get_name(rt_model3d_get_node_animation(obj, index));
}

/// @brief Borrow an imported Camera3D from @p scene_index (NULL on out-of-range).
/// @param obj Model3D receiver.
/// @param scene_index Zero-based immutable-scene index.
/// @param index Zero-based camera index within the scene.
/// @return Borrowed valid Camera3D, or NULL.
void *rt_model3d_get_camera(void *obj, int64_t scene_index, int64_t index) {
    rt_model3d *model = model3d_checked(obj);
    model3d_scene_entry *scene;
    int32_t scene_count = model_scene_count(model);
    int32_t camera_count;
    if (!model || scene_index < 0 || scene_index >= scene_count)
        return NULL;
    scene = &model->scenes[scene_index];
    camera_count =
        model_clamped_array_count(scene->cameras, scene->camera_count, scene->camera_capacity);
    if (index < 0 || index >= camera_count)
        return NULL;
    return rt_g3d_checked_or_null(scene->cameras[index], RT_G3D_CAMERA3D_CLASS_ID);
}

/// @brief Return the immutable scene name for @p index.
/// @details Invalid model handles or indices return the empty string.
/// @param obj Model3D receiver.
/// @param index Zero-based immutable-scene index.
/// @return Borrowed scene name, or an empty runtime string.
rt_string rt_model3d_get_scene_name(void *obj, int64_t index) {
    rt_model3d *model = model3d_checked(obj);
    int32_t scene_count = model_scene_count(model);
    if (!model || index < 0 || index >= scene_count || !model->scenes[index].name)
        return rt_const_cstr("");
    return rt_const_cstr(model->scenes[index].name);
}

/// @brief Number of KHR_materials_variants names imported with the asset (0 when absent).
/// @param obj Model3D receiver.
/// @return Imported variant-name count, or zero.
int64_t rt_model3d_get_variant_count(void *obj) {
    rt_model3d *model = model3d_checked(obj);
    if (!model || !model->variant_names || model->variant_count <= 0)
        return 0;
    return model->variant_count;
}

/// @brief Return the material-variant name at @p index (empty string when invalid).
/// @param obj Model3D receiver.
/// @param index Zero-based variant index.
/// @return Borrowed variant name, or an empty runtime string.
rt_string rt_model3d_get_variant_name(void *obj, int64_t index) {
    rt_model3d *model = model3d_checked(obj);
    if (!model || !model->variant_names || index < 0 || index >= model->variant_count ||
        !model->variant_names[index])
        return rt_const_cstr("");
    return rt_const_cstr(model->variant_names[index]);
}

/// @brief Apply material variant @p variant_index to every mapped node under @p target.
/// @details @p target may be a SceneNode3D subtree root (e.g. an Instantiate() result) or a
///          SceneGraph, in which case its root node is used. Nodes without a variant table
///          keep their current material; unmapped table slots were resolved to the
///          primitive's default material at import, so switching variants is reversible.
/// @param obj Model3D containing the imported variant table.
/// @param target SceneNode3D subtree root or Scene3D to update.
/// @param variant_index Zero-based imported variant index.
/// @return Number of nodes whose material was set (0 for invalid handles or indices).
int64_t rt_model3d_apply_variant(void *obj, void *target, int64_t variant_index) {
    rt_model3d *model = model3d_checked(obj);
    rt_scene_node3d *root = NULL;
    rt_scene_node3d **stack;
    int32_t stack_count = 0;
    int32_t stack_capacity = 32;
    int32_t visited_count = 1;
    int64_t applied = 0;
    if (!model || !target || variant_index < 0 || variant_index >= model->variant_count)
        return 0;
    root = (rt_scene_node3d *)rt_g3d_checked_or_null(target, RT_G3D_SCENENODE3D_CLASS_ID);
    if (!root) {
        rt_scene3d *scene = (rt_scene3d *)rt_g3d_checked_or_null(target, RT_G3D_SCENE3D_CLASS_ID);
        root = scene ? (rt_scene_node3d *)rt_g3d_checked_or_null(scene->root,
                                                                 RT_G3D_SCENENODE3D_CLASS_ID)
                     : NULL;
    }
    if (!root)
        return 0;
    stack = (rt_scene_node3d **)malloc((size_t)stack_capacity * sizeof(*stack));
    if (!stack) {
        rt_trap("SceneAsset.ApplyVariant: walk stack allocation failed");
        return 0;
    }
    stack[stack_count++] = root;
    while (stack_count > 0) {
        rt_scene_node3d *current = stack[--stack_count];
        int32_t child_count;
        if (!current)
            continue;
        if (current->variant_materials && variant_index < current->variant_material_count &&
            current->variant_materials[variant_index]) {
            rt_scene_node3d_set_material(current, current->variant_materials[variant_index]);
            applied++;
        }
        child_count = model_node_child_count(current);
        for (int32_t i = 0; i < child_count; i++) {
            rt_scene_node3d *child = (rt_scene_node3d *)rt_g3d_checked_or_null(
                current->children[i], RT_G3D_SCENENODE3D_CLASS_ID);
            if (!child)
                continue;
            if (visited_count >= MODEL3D_MAX_WALK_NODES) {
                free(stack);
                rt_trap("SceneAsset.ApplyVariant: too many nodes");
                return applied;
            }
            if (stack_count >= stack_capacity) {
                if (stack_capacity > INT32_MAX / 2 ||
                    (size_t)(stack_capacity * 2) > SIZE_MAX / sizeof(rt_scene_node3d *)) {
                    free(stack);
                    rt_trap("SceneAsset.ApplyVariant: too many nodes");
                    return applied;
                }
                int32_t new_capacity = stack_capacity * 2;
                rt_scene_node3d **grown = (rt_scene_node3d **)realloc(
                    stack, (size_t)new_capacity * sizeof(rt_scene_node3d *));
                if (!grown) {
                    free(stack);
                    rt_trap("SceneAsset.ApplyVariant: walk stack allocation failed");
                    return applied;
                }
                stack = grown;
                stack_capacity = new_capacity;
            }
            stack[stack_count++] = child;
            visited_count++;
        }
    }
    free(stack);
    return applied;
}

/// @brief One generated LOD chain shared by every node that renders the same mesh.
/// @details Instantiated scene clones share mesh pointers, so simplifying once per
///          unique mesh (instead of once per node) avoids duplicate QEM work and keeps
///          all consumers of a mesh on the identical decimated chain.
typedef struct {
    void *mesh;
    void *lods[4];
    double distances[4];
    int32_t count;
    uint32_t base_tris;    ///< Validated source triangle count (main thread).
    double first_distance; ///< Level-0 threshold from the mesh bounding radius.
    int8_t prepared;       ///< Nonzero once main-thread prep ran and decimation
                           ///< is worthwhile (>= 8 triangles).
} model_lod_chain_entry;

/// @brief One worker's slice of the parallel LOD chain build.
/// @details Workers stride the entry array (entry w, w+W, ...), so no two
///          workers ever touch the same entry or source mesh. All source-mesh
///          mutation (geometry repair, bounds refresh) happened on the main
///          thread during collection; workers only read sources and allocate
///          fresh decimated meshes through the thread-safe runtime heap.
typedef struct {
    model_lod_chain_entry *entries;
    int32_t entry_count;
    int32_t worker_index;
    int32_t worker_count;
    int64_t levels;
    double ratio;
} model_lod_build_slice_t;

/// @brief Build the LOD chain for one prepared entry (worker or inline).
/// @details Chained decimation: level k reduces level k-1 instead of the
///          base, cutting QEM cost by more than half on heavy scans while
///          producing the same visual ladder.
/// @param entry Prepared cache entry whose chain is filled in place.
/// @param levels Sanitized chain length.
/// @param ratio Per-level triangle ratio.
static void model_lod_chain_build_one(model_lod_chain_entry *entry, int64_t levels, double ratio) {
    void *source = entry->mesh;
    int64_t source_tris = (int64_t)entry->base_tris;
    double target = (double)entry->base_tris;
    double distance = entry->first_distance;
    if (!entry->prepared)
        return;
    for (int64_t k = 0; k < levels; k++) {
        int64_t tri_target;
        void *lod;
        rt_mesh3d *lod_mesh;
        int64_t lod_tris;
        if (source_tris <= 2)
            break;
        target *= ratio;
        tri_target = (int64_t)target;
        if (tri_target < 2)
            tri_target = 2;
        if (tri_target >= source_tris)
            tri_target = source_tris - 1;
        lod = rt_mesh3d_simplify(source, tri_target);
        if (!lod)
            break;
        lod_mesh = (rt_mesh3d *)rt_g3d_checked_or_null(lod, RT_G3D_MESH3D_CLASS_ID);
        lod_tris = lod_mesh ? (int64_t)(rt_mesh3d_validated_index_count(lod_mesh) / 3u) : 0;
        if (lod_tris <= 0 || lod_tris >= source_tris) {
            model_release_local(lod);
            break;
        }
        entry->lods[entry->count] = lod; /* construction ref released by the caller */
        entry->distances[entry->count] = distance;
        entry->count++;
        distance *= 2.0;
        source = lod;
        source_tris = lod_tris;
    }
}

/// @brief Thread entry: build every chain in this worker's stride.
/// @param arg Borrowed model_lod_build_slice_t owned by the dispatching frame.
static void model_lod_chain_build_slice(void *arg) {
    model_lod_build_slice_t *slice = (model_lod_build_slice_t *)arg;
    for (int32_t i = slice->worker_index; i < slice->entry_count; i += slice->worker_count)
        model_lod_chain_build_one(&slice->entries[i], slice->levels, slice->ratio);
}

/// @brief Find (or register and prepare) the shared cache entry for @p mesh_obj.
/// @details Main-thread half of chain generation: dedups by mesh pointer and
///          runs every source-mesh MUTATION (geometry-count repair, bounds
///          refresh) plus the triangle/radius reads, so the parallel build
///          phase only reads sources. Meshes under 8 triangles are marked
///          unprepared and keep an empty chain.
/// @param entries Address of the shared-chain cache array.
/// @param entry_count Address of the initialized cache-entry count.
/// @param entry_capacity Address of the cache-array capacity.
/// @param mesh_obj Mesh3D whose entry is fetched or prepared.
/// @return The cache entry for @p mesh_obj, or NULL on allocation failure.
static model_lod_chain_entry *model_lod_chain_find_or_prepare(model_lod_chain_entry **entries,
                                                              int32_t *entry_count,
                                                              int32_t *entry_capacity,
                                                              void *mesh_obj) {
    rt_mesh3d *mesh;
    model_lod_chain_entry *entry;
    uint32_t base_tris;
    double radius;
    for (int32_t i = 0; i < *entry_count; i++) {
        if ((*entries)[i].mesh == mesh_obj)
            return &(*entries)[i];
    }
    if (*entry_count >= *entry_capacity) {
        int32_t new_capacity;
        model_lod_chain_entry *grown;
        if (*entry_capacity > INT32_MAX / 2)
            return NULL;
        new_capacity = *entry_capacity > 0 ? *entry_capacity * 2 : 8;
        if ((size_t)new_capacity > SIZE_MAX / sizeof(model_lod_chain_entry))
            return NULL;
        grown = (model_lod_chain_entry *)realloc(*entries, (size_t)new_capacity * sizeof(*grown));
        if (!grown)
            return NULL;
        *entries = grown;
        *entry_capacity = new_capacity;
    }
    entry = &(*entries)[(*entry_count)++];
    memset(entry, 0, sizeof(*entry));
    entry->mesh = mesh_obj;
    mesh = (rt_mesh3d *)rt_g3d_checked_or_null(mesh_obj, RT_G3D_MESH3D_CLASS_ID);
    if (!mesh)
        return entry;
    rt_mesh3d_repair_geometry_counts(mesh);
    base_tris = rt_mesh3d_validated_index_count(mesh) / 3u;
    if (base_tris < 8u)
        return entry; /* nothing worth decimating */
    rt_mesh3d_refresh_bounds(mesh);
    radius = isfinite(mesh->bsphere_radius) && mesh->bsphere_radius > 0.0f
                 ? (double)mesh->bsphere_radius
                 : 1.0;
    entry->base_tris = base_tris;
    entry->first_distance = radius * 12.0;
    entry->prepared = 1;
    return entry;
}

/// @brief Generate shared LOD chains for every mesh node in the asset's templates.
/// @details Walks the template hierarchy and every immutable scene root, decimating each
///          unique mesh once (QEM subset placement, skin/UV-seam safe) into 1..4 levels of
///          repeatedly @p ratio-scaled triangles, then attaches the chain and enables auto
///          screen-error
///          selection. Nodes that already carry LOD chains are left untouched, so the call
///          is idempotent. Instantiate()/InstantiateScene() clones inherit the chains.
/// @param obj Model3D whose template and immutable scene roots are traversed.
/// @param levels Requested chain length, clamped from one through four.
/// @param ratio Per-level triangle ratio; invalid values use 0.4.
/// @return Number of nodes that received a LOD chain.
int64_t rt_model3d_generate_lods(void *obj, int64_t levels, double ratio) {
    rt_model3d *model = model3d_checked(obj);
    model_lod_chain_entry *chains = NULL;
    int32_t chain_count = 0;
    int32_t chain_capacity = 0;
    rt_scene_node3d **nodes = NULL;
    size_t node_count = 0;
    int64_t chained_nodes = 0;
    rt_scene_node3d **pending_nodes = NULL;
    int32_t *pending_entries = NULL;
    int32_t pending_count = 0;
    int32_t pending_capacity = 0;
    if (!model)
        return 0;
    if (levels < 1)
        levels = 1;
    if (levels > 4)
        levels = 4;
    if (!isfinite(ratio) || ratio <= 0.0 || ratio >= 1.0)
        ratio = 0.4;
    if (!model_collect_nodes(
            model, &nodes, &node_count, "SceneAsset.GenerateLODs: node walk allocation failed"))
        return 0;
    /* Phase 1 (main thread): walk, dedup meshes, run every source-mesh
     * mutation, and remember which node pairs with which cache entry. */
    for (size_t node_index = 0; node_index < node_count; node_index++) {
        rt_scene_node3d *current = nodes[node_index];
        if (current->mesh && model_node_lod_count(current) == 0) {
            model_lod_chain_entry *entry = model_lod_chain_find_or_prepare(
                &chains, &chain_count, &chain_capacity, current->mesh);
            if (!entry)
                goto phase1_fail;
            if (pending_count >= pending_capacity) {
                int32_t new_capacity;
                rt_scene_node3d **grown_nodes;
                int32_t *grown_entries;
                if (pending_capacity > INT32_MAX / 2)
                    goto phase1_fail;
                new_capacity = pending_capacity > 0 ? pending_capacity * 2 : 32;
                if ((size_t)new_capacity > SIZE_MAX / sizeof(*pending_nodes) ||
                    (size_t)new_capacity > SIZE_MAX / sizeof(*pending_entries))
                    goto phase1_fail;
                grown_nodes = (rt_scene_node3d **)realloc(
                    pending_nodes, (size_t)new_capacity * sizeof(*pending_nodes));
                if (!grown_nodes)
                    goto phase1_fail;
                pending_nodes = grown_nodes;
                grown_entries = (int32_t *)realloc(pending_entries,
                                                   (size_t)new_capacity * sizeof(*pending_entries));
                if (!grown_entries)
                    goto phase1_fail;
                pending_entries = grown_entries;
                pending_capacity = new_capacity;
            }
            pending_nodes[pending_count] = current;
            pending_entries[pending_count] = (int32_t)(entry - chains);
            pending_count++;
        }
    }
    free(nodes);
    /* Phase 2: decimate the unique meshes, striped across worker threads.
     * Chains are per-entry state and workers stride disjoint entries, so the
     * only shared machinery is the thread-safe runtime heap. Falls back to
     * inline builds when threads are unavailable or the batch is small. */
    if (chain_count > 0) {
        int64_t cores = rt_machine_cores();
        int64_t worker_count = cores > 1 ? cores - 1 : 1;
        int32_t workers;
        if (worker_count > chain_count)
            worker_count = chain_count;
        if (worker_count > 8)
            worker_count = 8;
        workers = (int32_t)worker_count;
        if (workers > 1) {
            model_lod_build_slice_t slices[8];
            void *threads[8];
            int32_t started = 0;
            for (int32_t w = 0; w < workers; w++) {
                slices[w].entries = chains;
                slices[w].entry_count = chain_count;
                slices[w].worker_index = w;
                slices[w].worker_count = workers;
                slices[w].levels = levels;
                slices[w].ratio = ratio;
            }
            /* Slice 0 runs on this thread; extras run on spawned threads. */
            for (int32_t w = 1; w < workers; w++) {
                threads[w] = rt_thread_start_fn(model_lod_chain_build_slice, &slices[w]);
                if (threads[w])
                    started++;
            }
            if (started == workers - 1) {
                model_lod_chain_build_slice(&slices[0]);
            } else {
                /* Some spawns failed: join what started, then finish every
                 * remaining entry inline (build_one skips filled chains via
                 * entry->count already being set only by its owner — chains
                 * touched by started workers are theirs alone, so rebuild
                 * only slices that never got a worker). */
                for (int32_t w = 1; w < workers; w++) {
                    if (threads[w])
                        rt_thread_join(threads[w]);
                }
                for (int32_t w = 0; w < workers; w++) {
                    int32_t had_thread = w == 0 ? 0 : (threads[w] ? 1 : 0);
                    if (!had_thread)
                        model_lod_chain_build_slice(&slices[w]);
                }
            }
            for (int32_t w = 1; w < workers; w++) {
                if (started == workers - 1 && threads[w])
                    rt_thread_join(threads[w]);
            }
        } else {
            for (int32_t i = 0; i < chain_count; i++)
                model_lod_chain_build_one(&chains[i], levels, ratio);
        }
    }
    /* Phase 3 (main thread): attach the shared chains to their nodes. */
    for (int32_t n = 0; n < pending_count; n++) {
        model_lod_chain_entry *entry = &chains[pending_entries[n]];
        if (entry->count > 0) {
            for (int32_t k = 0; k < entry->count; k++)
                rt_scene_node3d_add_lod(pending_nodes[n], entry->distances[k], entry->lods[k]);
            if (model_node_lod_count(pending_nodes[n]) == entry->count) {
                rt_scene_node3d_set_auto_lod(pending_nodes[n], 1, 3.0);
                chained_nodes++;
            } else {
                rt_scene_node3d_clear_lod(pending_nodes[n]);
            }
        }
    }
    free(pending_nodes);
    free(pending_entries);
    /* Nodes retained the LOD meshes through add_lod; drop the construction refs. */
    for (int32_t i = 0; i < chain_count; i++)
        for (int32_t k = 0; k < chains[i].count; k++)
            model_release_local(chains[i].lods[k]);
    free(chains);
    return chained_nodes;

phase1_fail:
    free(nodes);
    free(pending_nodes);
    free(pending_entries);
    free(chains);
    rt_trap("SceneAsset.GenerateLODs: node list allocation failed");
    return 0;
}

/// @brief Load a temporary Model3D and return one retained clip from it.
/// @param path Filesystem or asset-manager model path.
/// @param index Zero-based animation index.
/// @param asset_path Nonzero to load through the asset manager.
/// @param node_animation Nonzero to select NodeAnimation3D instead of skeletal Animation3D.
/// @return Retained standalone clip, or NULL on load failure or invalid index.
static void *model3d_load_clip_from_model(rt_string path,
                                          int64_t index,
                                          int asset_path,
                                          int node_animation) {
    void *model = asset_path ? rt_model3d_load_asset(path) : rt_model3d_load(path);
    void *clip = NULL;
    if (!model)
        return NULL;
    clip = node_animation ? rt_model3d_get_node_animation(model, index)
                          : rt_model3d_get_animation(model, index);
    if (clip)
        rt_obj_retain_maybe(clip);
    model_release_local(model);
    return clip;
}

/// @brief Load a model file and return a retained skeletal Animation3D clip by index.
/// @param path Filesystem model path.
/// @param index Zero-based skeletal-animation index.
/// @return Retained standalone Animation3D, or NULL.
void *rt_model3d_load_animation(rt_string path, int64_t index) {
    return model3d_load_clip_from_model(path, index, 0, 0);
}

/// @brief Load a skeletal animation clip as a Result.
/// @param path Filesystem model path.
/// @param index Zero-based skeletal animation index.
/// @return Owned Zanna.Result carrying Animation3D or an error string.
void *rt_model3d_load_animation_result(rt_string path, int64_t index) {
    return model3d_load_value_to_result(rt_model3d_load_animation(path, index),
                                        "SceneAsset.LoadAnimation failed");
}

/// @brief Load a packed model asset and return a retained skeletal Animation3D clip by index.
/// @param path Logical asset path or URI.
/// @param index Zero-based skeletal-animation index.
/// @return Retained standalone Animation3D, or NULL.
void *rt_model3d_load_animation_asset(rt_string path, int64_t index) {
    return model3d_load_clip_from_model(path, index, 1, 0);
}

/// @brief Load a skeletal animation clip from a packed asset as a Result.
/// @param path Asset path or URI.
/// @param index Zero-based skeletal animation index.
/// @return Owned Zanna.Result carrying Animation3D or an error string.
void *rt_model3d_load_animation_asset_result(rt_string path, int64_t index) {
    return model3d_load_value_to_result(rt_model3d_load_animation_asset(path, index),
                                        "SceneAsset.LoadAnimationAsset failed");
}

/// @brief Load a model file and return a retained NodeAnimation3D clip by index.
/// @param path Filesystem model path.
/// @param index Zero-based node-animation index.
/// @return Retained standalone NodeAnimation3D, or NULL.
void *rt_model3d_load_node_animation(rt_string path, int64_t index) {
    return model3d_load_clip_from_model(path, index, 0, 1);
}

/// @brief Load a node/object animation clip as a Result.
/// @param path Filesystem model path.
/// @param index Zero-based node animation index.
/// @return Owned Zanna.Result carrying NodeAnimation3D or an error string.
void *rt_model3d_load_node_animation_result(rt_string path, int64_t index) {
    return model3d_load_value_to_result(rt_model3d_load_node_animation(path, index),
                                        "SceneAsset.LoadNodeAnimation failed");
}

/// @brief Load a packed model asset and return a retained NodeAnimation3D clip by index.
/// @param path Logical asset path or URI.
/// @param index Zero-based node-animation index.
/// @return Retained standalone NodeAnimation3D, or NULL.
void *rt_model3d_load_node_animation_asset(rt_string path, int64_t index) {
    return model3d_load_clip_from_model(path, index, 1, 1);
}

/// @brief Load a node/object animation clip from a packed asset as a Result.
/// @param path Asset path or URI.
/// @param index Zero-based node animation index.
/// @return Owned Zanna.Result carrying NodeAnimation3D or an error string.
void *rt_model3d_load_node_animation_asset_result(rt_string path, int64_t index) {
    return model3d_load_value_to_result(rt_model3d_load_node_animation_asset(path, index),
                                        "SceneAsset.LoadNodeAnimationAsset failed");
}

/// @brief Locate a node in the template subtree by exact name match. NULL if not found.
/// @details The returned node belongs to the immutable template and must not be mutated or
/// released.
/// @param obj Model3D receiver.
/// @param name Exact node name to match.
/// @return Borrowed template SceneNode3D, or NULL.
void *rt_model3d_find_node(void *obj, rt_string name) {
    rt_model3d *model = model3d_checked(obj);
    if (!model || !model->template_root)
        return NULL;
    return model_find_node_safe(model->template_root, name);
}

/// @brief Locate a template node by exact name and return it as an Option.
/// @details This is the absence-aware companion to `rt_model3d_find_node`.
///          The found node is still part of the immutable imported template;
///          mutate instantiated nodes rather than the template node.
/// @param obj SceneAsset/Model3D handle.
/// @param name Exact node name to match.
/// @return `Some(SceneNode3D)` when the template contains the node, otherwise `None`.
void *rt_model3d_find_node_option(void *obj, rt_string name) {
    void *node = rt_model3d_find_node(obj, name);
    return node ? rt_option_some(node) : rt_option_none();
}

/// @brief One material group accumulated by `rt_model3d_flatten_static`.
typedef struct {
    void *material;       /* borrowed group key (may be NULL) */
    void *mesh;           /* owned merged Mesh3D */
    rt_string first_name; /* borrowed name of the first contributing node, or NULL */
} model3d_flatten_group;

/// @brief Row-major 4x4 multiply: out = a * b.
/// @param a Left matrix (16 doubles).
/// @param b Right matrix (16 doubles).
/// @param out Receives the product; must not alias the inputs.
static void model3d_mat4d_mul(const double *a, const double *b, double *out) {
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            out[r * 4 + c] = a[r * 4 + 0] * b[0 * 4 + c] + a[r * 4 + 1] * b[1 * 4 + c] +
                             a[r * 4 + 2] * b[2 * 4 + c] + a[r * 4 + 3] * b[3 * 4 + c];
        }
    }
}

/// @brief Upper-3x3 determinant of a row-major 4x4 matrix.
/// @param m Matrix (16 doubles).
/// @return The determinant of the linear part.
static double model3d_mat4d_det3(const double *m) {
    return m[0] * (m[5] * m[10] - m[6] * m[9]) - m[1] * (m[4] * m[10] - m[6] * m[8]) +
           m[2] * (m[4] * m[9] - m[5] * m[8]);
}

/// @brief Find or append the flatten group for a material.
/// @param groups Address of the owned group array.
/// @param count Address of the group count.
/// @param capacity Address of the group capacity.
/// @param material Borrowed group key.
/// @return Group slot, or NULL when growth fails.
static model3d_flatten_group *model3d_flatten_group_for(model3d_flatten_group **groups,
                                                        int32_t *count,
                                                        int32_t *capacity,
                                                        void *material) {
    for (int32_t i = 0; i < *count; i++) {
        if ((*groups)[i].material == material)
            return &(*groups)[i];
    }
    if (*count >= *capacity) {
        int32_t next = *capacity == 0 ? 8 : *capacity * 2;
        model3d_flatten_group *grown;
        if (next <= *capacity)
            return NULL;
        grown = (model3d_flatten_group *)realloc(*groups, (size_t)next * sizeof(*grown));
        if (!grown)
            return NULL;
        *groups = grown;
        *capacity = next;
    }
    (*groups)[*count].material = material;
    (*groups)[*count].mesh = NULL;
    (*groups)[*count].first_name = NULL;
    return &(*groups)[(*count)++];
}

void *rt_model3d_flatten_static(void *obj, rt_string root_name, void *transform) {
    rt_model3d *model = model3d_checked(obj);
    void *out = rt_seq_new_owned();
    rt_scene_node3d *root = NULL;
    rt_scene_node3d **stack = NULL;
    int32_t stack_count = 0;
    int32_t stack_capacity = 0;
    model3d_flatten_group *groups = NULL;
    int32_t group_count = 0;
    int32_t group_capacity = 0;
    int32_t skipped = 0;
    int failed = 0;
    double placement[16];
    const char *needle;
    if (!out)
        return NULL;
    if (!model || !model->template_root)
        return out;
    needle = model_string_cstr_or_null(root_name);
    if (needle && needle[0] != '\0') {
        root = model_find_node_safe(model->template_root, root_name);
        if (!root)
            return out;
    } else {
        root = model->template_root;
    }
    for (int i = 0; i < 16; i++)
        placement[i] = (i % 5 == 0) ? 1.0 : 0.0;
    if (transform) {
        for (int r = 0; r < 4; r++) {
            for (int c = 0; c < 4; c++) {
                double v = rt_mat4_get(transform, r, c);
                if (!isfinite(v)) {
                    rt_trap("SceneAsset.FlattenStatic: placement matrix must be finite");
                    return out;
                }
                placement[r * 4 + c] = v;
            }
        }
    }

    stack_capacity = 32;
    stack = (rt_scene_node3d **)malloc((size_t)stack_capacity * sizeof(*stack));
    if (!stack) {
        rt_trap("SceneAsset.FlattenStatic: traversal stack allocation failed");
        return out;
    }
    stack[stack_count++] = root;
    while (stack_count > 0 && !failed) {
        rt_scene_node3d *current = stack[--stack_count];
        int32_t child_count;
        if (!current)
            continue;
        child_count = model_node_child_count(current);
        /* Push children in reverse so they pop in authored order. */
        for (int32_t i = child_count - 1; i >= 0; i--) {
            rt_scene_node3d *child = current->children ? current->children[i] : NULL;
            if (!child)
                continue;
            if (stack_count >= stack_capacity) {
                int32_t next = stack_capacity * 2;
                rt_scene_node3d **grown;
                if (next <= stack_capacity) {
                    failed = 1;
                    break;
                }
                grown = (rt_scene_node3d **)realloc(stack, (size_t)next * sizeof(*grown));
                if (!grown) {
                    failed = 1;
                    break;
                }
                stack = grown;
                stack_capacity = next;
            }
            stack[stack_count++] = child;
        }
        if (failed)
            break;
        if (!current->visible || !current->mesh)
            continue;
        {
            rt_mesh3d *mesh = (rt_mesh3d *)current->mesh;
            double world[16];
            double composed[16];
            void *clone;
            model3d_flatten_group *group;
            if (mesh->skeleton_ref || mesh->morph_targets_ref) {
                skipped++;
                continue;
            }
            if (!rt_scene_node3d_get_world_matrix_components(current, world)) {
                skipped++;
                continue;
            }
            model3d_mat4d_mul(placement, world, composed);
            if (fabs(model3d_mat4d_det3(composed)) <= 1e-12) {
                skipped++;
                continue;
            }
            group = model3d_flatten_group_for(
                &groups, &group_count, &group_capacity, current->material);
            if (!group) {
                failed = 1;
                break;
            }
            clone = rt_mesh3d_clone(mesh);
            if (!clone) {
                failed = 1;
                break;
            }
            rt_mesh3d_transform_components(clone, composed);
            if (!group->mesh) {
                group->mesh = clone;
                group->first_name = current->name;
            } else {
                rt_mesh3d_append(group->mesh, clone);
                model_release_local(clone);
            }
        }
    }
    free(stack);

    if (failed) {
        for (int32_t i = 0; i < group_count; i++)
            model_release_local(groups[i].mesh);
        free(groups);
        rt_trap("SceneAsset.FlattenStatic: allocation failed while merging meshes");
        return out;
    }
    for (int32_t i = 0; i < group_count; i++) {
        void *node;
        char name[32];
        rt_string node_name;
        if (!groups[i].mesh)
            continue;
        node = rt_scene_node3d_new();
        if (!node) {
            model_release_local(groups[i].mesh);
            continue;
        }
        /* The group carries the name of its first contributing node so a
         * name-based role convention (tint_*, emissive_*) survives flattening;
         * unnamed sources fall back to the group index. */
        if (groups[i].first_name && model_string_cstr_or_null(groups[i].first_name) &&
            model_string_cstr_or_null(groups[i].first_name)[0] != '\0') {
            rt_scene_node3d_set_name(node, groups[i].first_name);
        } else {
            snprintf(name, sizeof(name), "flatten_%d", (int)i);
            node_name = rt_const_cstr(name);
            rt_scene_node3d_set_name(node, node_name);
            rt_string_unref(node_name);
        }
        rt_scene_node3d_set_mesh(node, groups[i].mesh);
        rt_scene_node3d_set_material(node, groups[i].material);
        model_release_local(groups[i].mesh);
        rt_seq_push(out, node);
        model_release_local(node);
    }
    free(groups);
    if (skipped > 0)
        rt_asset_error_add_warningf("SceneAsset.FlattenStatic: %d node%s skipped (skinned, morph, "
                                    "or singular transform)",
                                    (int)skipped,
                                    skipped == 1 ? "" : "s");
    return out;
}

/// @brief Clone the template into an independent SceneNode3D subtree (with synthetic root).
/// Each instantiation creates a fresh deep-copy of the node hierarchy, suitable for adding to
/// a live scene without disturbing the template. Static meshes/materials are shared; meshes
/// with attached morph targets are cloned so their blend-shape weights are instance-local.
/// @param obj Model3D receiver.
/// @return New independently mutable synthetic-root SceneNode3D subtree, or NULL on failure.
void *rt_model3d_instantiate(void *obj) {
    rt_model3d *model = model3d_checked(obj);
    rt_scene_node3d *root;
    if (!model || !model->template_root)
        return NULL;
    root = model_clone_node(model->template_root, 1);
    model_bind_default_animator(model, root);
    model_bind_default_node_animator(model, root);
    return root;
}

/// @brief Build a fresh Scene3D from the template's children, cloning each one. Use when the
/// caller wants the model exposed as a top-level scene rather than a node subtree.
/// @param obj Model3D receiver.
/// @return New default Scene3D instance, or NULL on failure.
void *rt_model3d_instantiate_scene(void *obj) {
    return rt_model3d_instantiate_scene_at(obj, 0);
}

/// @brief Build a fresh Scene3D for immutable scene @p index.
/// @param obj Model3D receiver.
/// @param index Zero-based immutable-scene index.
/// @return New independently mutable Scene3D, or NULL for invalid input or cloning failure.
void *rt_model3d_instantiate_scene_at(void *obj, int64_t index) {
    rt_model3d *model = model3d_checked(obj);
    rt_scene3d *scene;
    model3d_scene_entry *entry;
    int32_t scene_count = model_scene_count(model);
    if (!model || index < 0 || index >= scene_count)
        return NULL;
    entry = &model->scenes[index];
    if (!entry->root)
        return NULL;
    scene = (rt_scene3d *)rt_scene3d_new();
    if (!scene)
        return NULL;
    /* Scene-scoped root metadata (bake.*, env.* conventions) travels with
     * every instantiation, matching direct SceneGraph.Load behavior. */
    if (!model_clone_node_metadata(scene->root, entry->root)) {
        model_release_local(scene);
        return NULL;
    }
    for (int32_t i = 0, child_count = model_node_child_count(entry->root); i < child_count; i++) {
        if (!entry->root->children[i])
            continue;
        rt_scene_node3d *child = model_clone_node(entry->root->children[i], 1);
        if (child) {
            if (!rt_scene3d_try_add(scene, child)) {
                model_release_local(child);
                model_release_local(scene);
                return NULL;
            }
            model_release_local(child);
        } else {
            model_release_local(scene);
            return NULL;
        }
    }
    model_bind_default_animator(model, scene->root);
    model_bind_default_node_animator(model, scene->root);
    /* Carry the model's clips on the scene so a baked .vscn save (v3) can
     * serialize the full rig; the handles are retained per-slot. */
    if (model->animation_count > 0 && model->animations) {
        scene->baked_animations = (void **)calloc((size_t)model->animation_count, sizeof(void *));
        if (scene->baked_animations) {
            for (int32_t i = 0; i < model->animation_count; i++) {
                scene->baked_animations[i] = model->animations[i];
                rt_obj_retain_maybe(model->animations[i]);
            }
            scene->baked_animation_count = model->animation_count;
        }
    }
    return scene;
}
