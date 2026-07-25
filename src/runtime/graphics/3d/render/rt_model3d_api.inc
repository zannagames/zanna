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
void *rt_model3d_load_text_result(rt_string path, rt_string text) {
    return model3d_load_value_to_result(model3d_load_text_value(path, text),
                                        "SceneAsset.LoadTextResult failed");
}

/// @brief Save the complete Model3D inventory through the model-aware VSCN v5 serializer.
int64_t rt_model3d_save(void *obj, rt_string path) {
    rt_model3d *model = model3d_checked(obj);
    rt_vscn_asset_save_view view = {0};
    rt_vscn_asset_scene_view *scenes = NULL;
    int32_t scene_count;
    int64_t result;
    if (!model || !path || !rt_string_is_handle(path) || !rt_string_cstr(path))
        return 0;
    scene_count = model_scene_count(model);
    if (scene_count <= 0 || !model->template_root)
        return 0;
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

/// @brief Load a model with explicit import options (Plan 09).
/// @details `force_tangents` generates tangents for every UV0-mapped glTF primitive even
///   when its material has no normal map at load time — for assets whose materials gain
///   normal maps after import. FBX ignores the option. Options scope thread-locally
///   around the load, so the default entry points are unchanged.
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
void *rt_model3d_load_result_with_options(rt_string path, int8_t force_tangents) {
    return model3d_load_value_to_result(rt_model3d_load_with_options(path, force_tangents),
                                        "SceneAsset.LoadWithOptions failed");
}

/// @brief Load a model through the asset manager (mounted/embedded + dev fallback). See header.
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
int64_t rt_model3d_get_mesh_count(void *obj) {
    rt_model3d *model = model3d_checked(obj);
    return model ? model_clamped_array_count(model->meshes, model->mesh_count, model->mesh_capacity)
                 : 0;
}

/// @brief Number of materials loaded into this model.
int64_t rt_model3d_get_material_count(void *obj) {
    rt_model3d *model = model3d_checked(obj);
    return model ? model_clamped_array_count(
                       model->materials, model->material_count, model->material_capacity)
                 : 0;
}

/// @brief Number of skeletons (bone hierarchies) loaded into this model.
int64_t rt_model3d_get_skeleton_count(void *obj) {
    rt_model3d *model = model3d_checked(obj);
    return model ? model_clamped_array_count(
                       model->skeletons, model->skeleton_count, model->skeleton_capacity)
                 : 0;
}

/// @brief Number of animation clips loaded into this model.
int64_t rt_model3d_get_animation_count(void *obj) {
    rt_model3d *model = model3d_checked(obj);
    return model ? model_clamped_array_count(
                       model->animations, model->animation_count, model->animation_capacity)
                 : 0;
}

/// @brief Number of node/object/morph/camera animation clips loaded into this model.
int64_t rt_model3d_get_node_animation_count(void *obj) {
    rt_model3d *model = model3d_checked(obj);
    return model ? model_clamped_array_count(model->node_animations,
                                             model->node_animation_count,
                                             model->node_animation_capacity)
                 : 0;
}

/// @brief Number of model mesh slots carrying a valid attached MorphTarget3D.
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
int64_t rt_model3d_get_scene_count(void *obj) {
    rt_model3d *model = model3d_checked(obj);
    return model_scene_count(model);
}

/// @brief Number of cameras imported for @p scene_index.
/// @details glTF cameras are retained per immutable scene. Invalid scene indices and importers
///   without authored cameras return zero.
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
void *rt_model3d_get_morph_target(void *obj, int64_t mesh_index) {
    rt_mesh3d *mesh = (rt_mesh3d *)rt_model3d_get_mesh(obj, mesh_index);
    return mesh ? rt_g3d_checked_or_null(mesh->morph_targets_ref, RT_G3D_MORPHTARGET3D_CLASS_ID)
                : NULL;
}

/// @brief Borrow the i-th NodeAnimation3D name, or an empty string on out-of-range.
rt_string rt_model3d_get_node_animation_name(void *obj, int64_t index) {
    return rt_node_animation3d_get_name(rt_model3d_get_node_animation(obj, index));
}

/// @brief Borrow an imported Camera3D from @p scene_index (NULL on out-of-range).
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
rt_string rt_model3d_get_scene_name(void *obj, int64_t index) {
    rt_model3d *model = model3d_checked(obj);
    int32_t scene_count = model_scene_count(model);
    if (!model || index < 0 || index >= scene_count || !model->scenes[index].name)
        return rt_const_cstr("");
    return rt_const_cstr(model->scenes[index].name);
}

/// @brief Number of KHR_materials_variants names imported with the asset (0 when absent).
int64_t rt_model3d_get_variant_count(void *obj) {
    rt_model3d *model = model3d_checked(obj);
    if (!model || !model->variant_names || model->variant_count <= 0)
        return 0;
    return model->variant_count;
}

/// @brief Return the material-variant name at @p index (empty string when invalid).
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
} model_lod_chain_entry;

/// @brief Build (or fetch) the shared LOD chain for @p mesh_obj inside @p entries.
/// @details Mirrors rt_scene_node3d_generate_lods' decimation schedule: level k holds
///          ~ratio^k of the source triangles and distance thresholds derive from the
///          mesh bounding radius. Meshes under 8 triangles produce an empty chain.
/// @return The cache entry for @p mesh_obj, or NULL on allocation failure.
static model_lod_chain_entry *model_lod_chain_find_or_build(model_lod_chain_entry **entries,
                                                            int32_t *entry_count,
                                                            int32_t *entry_capacity,
                                                            void *mesh_obj,
                                                            int64_t levels,
                                                            double ratio) {
    rt_mesh3d *mesh;
    model_lod_chain_entry *entry;
    uint32_t base_tris;
    double radius;
    double target;
    double distance;
    for (int32_t i = 0; i < *entry_count; i++) {
        if ((*entries)[i].mesh == mesh_obj)
            return &(*entries)[i];
    }
    if (*entry_count >= *entry_capacity) {
        int32_t new_capacity = *entry_capacity > 0 ? *entry_capacity * 2 : 8;
        model_lod_chain_entry *grown;
        if (*entry_capacity > INT32_MAX / 2 ||
            (size_t)new_capacity > SIZE_MAX / sizeof(model_lod_chain_entry))
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
    radius = mesh->bsphere_radius > 0.0f ? (double)mesh->bsphere_radius : 1.0;
    target = (double)base_tris;
    distance = radius * 12.0;
    for (int64_t k = 0; k < levels; k++) {
        int64_t tri_target;
        void *lod;
        target *= ratio;
        tri_target = (int64_t)target;
        if (tri_target < 2)
            tri_target = 2;
        lod = rt_mesh3d_simplify(mesh_obj, tri_target);
        if (!lod)
            break;
        entry->lods[entry->count] = lod; /* construction ref released by the caller */
        entry->distances[entry->count] = distance;
        entry->count++;
        distance *= 2.0;
    }
    return entry;
}

/// @brief Generate shared LOD chains for every mesh node in the asset's templates.
/// @details Walks the template hierarchy and every immutable scene root, decimating each
///          unique mesh once (QEM subset placement, skin/UV-seam safe) into 1..4 levels of
///          ~@p ratio^k triangles, then attaches the chain and enables auto screen-error
///          selection. Nodes that already carry LOD chains are left untouched, so the call
///          is idempotent. Instantiate()/InstantiateScene() clones inherit the chains.
/// @return Number of nodes that received a LOD chain.
int64_t rt_model3d_generate_lods(void *obj, int64_t levels, double ratio) {
    rt_model3d *model = model3d_checked(obj);
    model_lod_chain_entry *chains = NULL;
    int32_t chain_count = 0;
    int32_t chain_capacity = 0;
    rt_scene_node3d **stack;
    int32_t stack_count = 0;
    int32_t stack_capacity = 32;
    int32_t visited_count = 0;
    int64_t chained_nodes = 0;
    int32_t scene_count;
    if (!model)
        return 0;
    if (levels < 1)
        levels = 1;
    if (levels > 4)
        levels = 4;
    if (!isfinite(ratio) || ratio <= 0.0 || ratio >= 1.0)
        ratio = 0.4;
    stack = (rt_scene_node3d **)malloc((size_t)stack_capacity * sizeof(*stack));
    if (!stack) {
        rt_trap("SceneAsset.GenerateLODs: walk stack allocation failed");
        return 0;
    }
    if (model->template_root)
        stack[stack_count++] = model->template_root;
    scene_count = model_scene_count(model);
    for (int32_t si = 0; si < scene_count; si++) {
        rt_scene_node3d *scene_root = (rt_scene_node3d *)rt_g3d_checked_or_null(
            model->scenes[si].root, RT_G3D_SCENENODE3D_CLASS_ID);
        /* Scene 0 aliases the template root; the per-node lod_count guard below also
         * keeps a re-visit harmless. */
        if (!scene_root || scene_root == model->template_root)
            continue;
        if (stack_count >= stack_capacity)
            break; /* unreachable with the initial capacity; scenes are few */
        stack[stack_count++] = scene_root;
    }
    while (stack_count > 0) {
        rt_scene_node3d *current = stack[--stack_count];
        int32_t child_count;
        if (!current)
            continue;
        if (++visited_count >= MODEL3D_MAX_WALK_NODES) {
            free(stack);
            model_lod_chain_entry *entries = chains;
            for (int32_t i = 0; i < chain_count; i++)
                for (int32_t k = 0; k < entries[i].count; k++)
                    model_release_local(entries[i].lods[k]);
            free(chains);
            rt_trap("SceneAsset.GenerateLODs: too many nodes");
            return chained_nodes;
        }
        if (current->mesh && model_node_lod_count(current) == 0) {
            model_lod_chain_entry *entry = model_lod_chain_find_or_build(
                &chains, &chain_count, &chain_capacity, current->mesh, levels, ratio);
            if (entry && entry->count > 0) {
                for (int32_t k = 0; k < entry->count; k++)
                    rt_scene_node3d_add_lod(current, entry->distances[k], entry->lods[k]);
                rt_scene_node3d_set_auto_lod(current, 1, 3.0);
                chained_nodes++;
            }
        }
        child_count = model_node_child_count(current);
        for (int32_t i = 0; i < child_count; i++) {
            rt_scene_node3d *child = (rt_scene_node3d *)rt_g3d_checked_or_null(
                current->children[i], RT_G3D_SCENENODE3D_CLASS_ID);
            if (!child)
                continue;
            if (stack_count >= stack_capacity) {
                int32_t new_capacity;
                rt_scene_node3d **grown;
                if (stack_capacity > INT32_MAX / 2 ||
                    (size_t)(stack_capacity * 2) > SIZE_MAX / sizeof(rt_scene_node3d *))
                    break;
                new_capacity = stack_capacity * 2;
                grown = (rt_scene_node3d **)realloc(stack, (size_t)new_capacity * sizeof(*stack));
                if (!grown)
                    break;
                stack = grown;
                stack_capacity = new_capacity;
            }
            stack[stack_count++] = child;
        }
    }
    free(stack);
    /* Nodes retained the LOD meshes through add_lod; drop the construction refs. */
    for (int32_t i = 0; i < chain_count; i++)
        for (int32_t k = 0; k < chains[i].count; k++)
            model_release_local(chains[i].lods[k]);
    free(chains);
    return chained_nodes;
}

/// @brief Load a temporary Model3D and return one retained clip from it.
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

/// @brief Clone the template into an independent SceneNode3D subtree (with synthetic root).
/// Each instantiation creates a fresh deep-copy of the node hierarchy, suitable for adding to
/// a live scene without disturbing the template. Static meshes/materials are shared; meshes
/// with attached morph targets are cloned so their blend-shape weights are instance-local.
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
void *rt_model3d_instantiate_scene(void *obj) {
    return rt_model3d_instantiate_scene_at(obj, 0);
}

/// @brief Build a fresh Scene3D for immutable scene @p index.
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
