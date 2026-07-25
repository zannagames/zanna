//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/render/rt_model3d_loaders.inc
// Purpose: SceneAsset import bridges for VSCN, OBJ, STL, glTF, and FBX sources.
// Key invariants:
//   - LoadAsset resolves root bytes and dependencies through the asset manager.
//   - Temporary spill files never replace the original logical dependency base path.
// Ownership/Lifetime:
//   - This implementation fragment is included by rt_model3d.c.
//   - Temporary bytes, paths, and files are released before each loader returns.
// Links: rt_model3d.c, rt_asset.h, rt_scene3d.h, rt_tempfile.h
//
//===----------------------------------------------------------------------===//

static int model3d_write_temp_asset_bytes(rt_string path, const uint8_t *bytes, size_t size);
static rt_string model3d_spill_asset_bytes(const char *prefix_cstr,
                                           const char *suffix_cstr,
                                           const uint8_t *bytes,
                                           size_t size);

/// @brief Extract the next token from *@p cursor — honoring single/double quotes so paths or
///   material names containing spaces survive — and advance @p cursor past it (and any closing
///   quote).
/// @return A malloc'd copy of the token, or NULL at end-of-line / on a comment.
static char *model_obj_next_path_token(const char **cursor) {
    const char *start;
    const char *end;
    char quote = '\0';
    if (!cursor || !*cursor)
        return NULL;
    while (**cursor == ' ' || **cursor == '\t')
        (*cursor)++;
    if (**cursor == '\0' || **cursor == '\n' || **cursor == '\r' || **cursor == '#')
        return NULL;
    start = *cursor;
    if (*start == '"' || *start == '\'') {
        quote = *start;
        start++;
        end = start;
        while (*end && *end != quote && *end != '\n' && *end != '\r')
            end++;
        *cursor = (*end == quote) ? end + 1 : end;
        return model_strdup_range(start, end);
    }
    end = start;
    while (*end && *end != ' ' && *end != '\t' && *end != '\n' && *end != '\r' && *end != '#')
        end++;
    *cursor = end;
    return model_strdup_range(start, end);
}

/// @brief Load the texture named by an MTL map directive (sanitized via the path-traversal
///   guard and joined to @p mtl_dir) and assign it to @p material's @p slot
///   (normal/specular/emissive, else base color). No-op on a missing/unsafe/unloadable path.
static void model_obj_set_texture_from_map(
    void *material, const char *mtl_dir, const char *map_value, int32_t slot, int load_assets) {
    char *map_ref;
    char safe_ref[MODEL3D_OBJ_MAX_PATH_BYTES];
    char *path;
    void *pixels;
    if (!material)
        return;
    map_ref = model_obj_extract_map_path(map_value);
    if (!map_ref)
        return;
    if (!model_normalize_relative_asset_ref(map_ref, safe_ref, sizeof(safe_ref))) {
        rt_asset_error_add_warningf("material texture '%s' ignored: unsafe relative path", map_ref);
        free(map_ref);
        return;
    }
    free(map_ref);
    path = model_join_path_alloc(mtl_dir, safe_ref);
    if (!path) {
        rt_asset_error_add_warningf("material texture '%s' ignored: path allocation failed",
                                    safe_ref);
        return;
    }
    {
        rt_string runtime_path = rt_const_cstr(path);
        pixels = load_assets ? rt_asset_load(runtime_path) : rt_pixels_load(runtime_path);
        rt_string_unref(runtime_path);
    }
    if (pixels && rt_obj_class_id(pixels) != RT_PIXELS_CLASS_ID) {
        model_release_local(pixels);
        pixels = NULL;
    }
    if (!pixels) {
        rt_asset_error_add_warningf("material texture '%s' not loaded: %s",
                                    safe_ref,
                                    rt_asset_error_get_message()[0] ? rt_asset_error_get_message()
                                                                    : "content unavailable");
        free(path);
        return;
    }
    free(path);
    switch (slot) {
        case RT_MATERIAL3D_TEXTURE_SLOT_NORMAL:
            rt_material3d_set_normal_map(material, pixels);
            break;
        case RT_MATERIAL3D_TEXTURE_SLOT_SPECULAR:
            rt_material3d_set_specular_map(material, pixels);
            break;
        case RT_MATERIAL3D_TEXTURE_SLOT_EMISSIVE:
            rt_material3d_set_emissive_map(material, pixels);
            break;
        default:
            rt_material3d_set_texture(material, pixels);
            break;
    }
    model_release_local(pixels);
}

/// @brief Parse the .mtl library referenced by @p mtllib_ref (relative to @p obj_dir), adding
///   each "newmtl" material — with its colors and map_* textures — to @p table.
static void model_obj_parse_mtl_file(model_obj_material_table *table,
                                     const char *obj_dir,
                                     const char *mtllib_ref,
                                     int load_assets) {
    char safe_ref[MODEL3D_OBJ_MAX_PATH_BYTES];
    char mtl_dir[MODEL3D_OBJ_MAX_PATH_BYTES];
    char *path;
    FILE *f = NULL;
    rt_string temp_path = NULL;
    char *line = NULL;
    size_t line_cap = 0;
    int line_status = 0;
    char *current_name = NULL;
    void *current_material = NULL;
    if (!table || !model_normalize_relative_asset_ref(mtllib_ref, safe_ref, sizeof(safe_ref)))
        return;
    path = model_join_path_alloc(obj_dir, safe_ref);
    if (!path)
        return;
    model_parent_dir(mtl_dir, sizeof(mtl_dir), path);
    if (load_assets) {
        size_t asset_size = 0;
        rt_string runtime_path = rt_const_cstr(path);
        uint8_t *asset_bytes = rt_asset_load_raw(runtime_path, &asset_size);
        rt_string_unref(runtime_path);
        if (asset_bytes) {
            temp_path =
                model3d_spill_asset_bytes("zanna_obj_mtl_", ".mtl", asset_bytes, asset_size);
            free(asset_bytes);
            if (temp_path)
                f = rt_file_stdio_open_utf8(rt_string_cstr(temp_path), "rb");
        }
    }
    if (!f)
        f = rt_file_stdio_open_utf8(path, "rb");
    if (!f) {
        rt_asset_error_add_warningf("material library '%s' not found", safe_ref);
        if (temp_path) {
            rt_io_file_delete(temp_path);
            rt_string_unref(temp_path);
        }
        free(path);
        return;
    }
    while ((line_status = model_read_text_line(f, &line, &line_cap, MODEL3D_OBJ_MAX_LINE_BYTES)) >
           0) {
        const char *p = line;
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == '#' || *p == '\0' || *p == '\n' || *p == '\r')
            continue;
        if (strncmp(p, "newmtl ", 7) == 0) {
            const char *cursor = p + 7;
            if (current_name && current_material &&
                !model_obj_material_table_append(table, current_name, current_material)) {
                free(current_name);
                model_release_local(current_material);
            }
            current_name = model_obj_next_path_token(&cursor);
            current_material = rt_material3d_new();
            if (!current_name || !current_material) {
                free(current_name);
                model_release_local(current_material);
                current_name = NULL;
                current_material = NULL;
            }
        } else if (current_material && strncmp(p, "Kd ", 3) == 0) {
            double r, g, b;
            p += 3;
            if (model_obj_parse_double3(p, &r, &g, &b))
                rt_material3d_set_color(current_material, r, g, b);
        } else if (current_material && strncmp(p, "Ks ", 3) == 0) {
            double r, g, b;
            p += 3;
            if (model_obj_parse_double3(p, &r, &g, &b)) {
                ((rt_material3d *)current_material)->specular[0] = model_obj_clamp01(r);
                ((rt_material3d *)current_material)->specular[1] = model_obj_clamp01(g);
                ((rt_material3d *)current_material)->specular[2] = model_obj_clamp01(b);
            }
        } else if (current_material && strncmp(p, "Ke ", 3) == 0) {
            double r, g, b;
            p += 3;
            if (model_obj_parse_double3(p, &r, &g, &b))
                rt_material3d_set_emissive_color(current_material, r, g, b);
        } else if (current_material && strncmp(p, "Ns ", 3) == 0) {
            double ns;
            p += 3;
            if (model_obj_parse_double1(p, &ns))
                rt_material3d_set_shininess(current_material, ns);
        } else if (current_material && strncmp(p, "map_Kd ", 7) == 0) {
            model_obj_set_texture_from_map(current_material,
                                           mtl_dir,
                                           p + 7,
                                           RT_MATERIAL3D_TEXTURE_SLOT_BASE_COLOR,
                                           load_assets);
        } else if (current_material && strncmp(p, "map_Ks ", 7) == 0) {
            model_obj_set_texture_from_map(
                current_material, mtl_dir, p + 7, RT_MATERIAL3D_TEXTURE_SLOT_SPECULAR, load_assets);
        } else if (current_material && strncmp(p, "map_Ke ", 7) == 0) {
            model_obj_set_texture_from_map(
                current_material, mtl_dir, p + 7, RT_MATERIAL3D_TEXTURE_SLOT_EMISSIVE, load_assets);
        } else if (current_material &&
                   (strncmp(p, "map_Bump ", 9) == 0 || strncmp(p, "map_bump ", 9) == 0 ||
                    strncmp(p, "bump ", 5) == 0 || strncmp(p, "norm ", 5) == 0)) {
            const char *map_value = (p[0] == 'b' || p[0] == 'n') ? p + 5 : p + 9;
            model_obj_set_texture_from_map(current_material,
                                           mtl_dir,
                                           map_value,
                                           RT_MATERIAL3D_TEXTURE_SLOT_NORMAL,
                                           load_assets);
        } else if (current_material && (strncmp(p, "d ", 2) == 0 || strncmp(p, "Tr ", 3) == 0)) {
            double a;
            const char *value = p + (p[0] == 'T' ? 3 : 2);
            if (model_obj_parse_double1(value, &a)) {
                if (p[0] == 'T')
                    a = 1.0 - a;
                rt_material3d_set_alpha(current_material, a);
            }
        }
    }
    fclose(f);
    if (temp_path) {
        rt_io_file_delete(temp_path);
        rt_string_unref(temp_path);
    }
    free(path);
    if (current_name && current_material &&
        !model_obj_material_table_append(table, current_name, current_material)) {
        free(current_name);
        model_release_local(current_material);
    }
    if (line_status < 0) {
        rt_asset_error_add_warning(line_status == -2 ? "OBJ material library ignored: line too long"
                                                     : "OBJ material library ignored: read failed");
    }
    free(line);
}

/// @brief Scan the OBJ file at @p obj_path for "mtllib" directives and parse each referenced
///   .mtl library into @p table.
static void model_obj_load_material_libraries(const char *obj_scan_path,
                                              const char *logical_obj_path,
                                              model_obj_material_table *table,
                                              int load_assets) {
    char obj_dir[MODEL3D_OBJ_MAX_PATH_BYTES];
    FILE *f;
    char *line = NULL;
    size_t line_cap = 0;
    int line_status = 0;
    if (!obj_scan_path || !table)
        return;
    model_parent_dir(obj_dir, sizeof(obj_dir), logical_obj_path ? logical_obj_path : obj_scan_path);
    f = rt_file_stdio_open_utf8(obj_scan_path, "rb");
    if (!f)
        return;
    while ((line_status = model_read_text_line(f, &line, &line_cap, MODEL3D_OBJ_MAX_LINE_BYTES)) >
           0) {
        const char *p = line;
        while (*p == ' ' || *p == '\t')
            p++;
        if (strncmp(p, "mtllib ", 7) == 0) {
            const char *cursor = p + 7;
            for (;;) {
                char *name = model_obj_next_path_token(&cursor);
                if (!name)
                    break;
                if (name) {
                    model_obj_parse_mtl_file(table, obj_dir, name, load_assets);
                    free(name);
                }
            }
        }
    }
    if (line_status < 0) {
        rt_asset_error_add_warning(line_status == -2 ? "OBJ mtllib directive ignored: line too long"
                                                     : "OBJ mtllib directive ignored: read failed");
    }
    free(line);
    fclose(f);
}

/// @brief Load a Wavefront OBJ file into @p model: parse vertices/normals/UVs and faces
///   (grouped by material), resolve materials from referenced .mtl libraries, and build the
///   model's per-material meshes. @return 1 on success, 0 on failure.
static int model3d_load_from_obj(rt_model3d *model,
                                 rt_string path,
                                 const char *path_cstr,
                                 int load_assets) {
    rt_mesh3d_obj_group_t *groups = NULL;
    int32_t group_count = 0;
    model_obj_material_table materials;
    void *default_material = NULL;
    rt_string geometry_path = path;
    rt_string temp_path = NULL;
    int ok = 0;
    memset(&materials, 0, sizeof(materials));
    if (load_assets) {
        size_t asset_size = 0;
        uint8_t *asset_bytes = rt_asset_load_raw(path, &asset_size);
        if (asset_bytes) {
            temp_path = model3d_spill_asset_bytes("zanna_obj_", ".obj", asset_bytes, asset_size);
            free(asset_bytes);
            if (!temp_path) {
                rt_asset_error_set_if_empty(RT_ASSET_ERROR_UNREADABLE,
                                            "Model3D.LoadAsset: failed to stage OBJ bytes");
                goto cleanup;
            }
            geometry_path = temp_path;
        }
    }
    if (!rt_mesh3d_from_obj_groups(geometry_path, &groups, &group_count) || group_count <= 0)
        goto cleanup;
    model_obj_load_material_libraries(
        rt_string_cstr(geometry_path), path_cstr, &materials, load_assets);
    default_material = rt_material3d_new();
    if (!default_material)
        goto cleanup;
    for (int32_t i = 0; i < group_count; i++) {
        void *material = model_obj_material_lookup(&materials, groups[i].material_name);
        rt_scene_node3d *node;
        char fallback_name[64];
        const char *node_name = groups[i].material_name && groups[i].material_name[0] != '\0'
                                    ? groups[i].material_name
                                    : NULL;
        if (!material)
            material = default_material;
        if (!model_append_unique_ref(&model->meshes,
                                     &model->mesh_count,
                                     &model->mesh_capacity,
                                     groups[i].mesh,
                                     "Model3D.Load: mesh list allocation failed") ||
            !model_append_unique_ref(&model->materials,
                                     &model->material_count,
                                     &model->material_capacity,
                                     material,
                                     "Model3D.Load: material list allocation failed")) {
            goto cleanup;
        }
        node = (rt_scene_node3d *)rt_scene_node3d_new();
        if (!node)
            goto cleanup;
        if (!node_name) {
            snprintf(fallback_name, sizeof(fallback_name), "mesh_%d", (int)i);
            node_name = fallback_name;
        }
        {
            rt_string runtime_name = rt_const_cstr(node_name);
            rt_scene_node3d_set_name(node, runtime_name);
            rt_string_unref(runtime_name);
        }
        rt_scene_node3d_set_mesh(node, groups[i].mesh);
        rt_scene_node3d_set_material(node, material);
        if (!rt_scene_node3d_try_add_child(model->template_root, node)) {
            model_release_local(node);
            goto cleanup;
        }
        model_release_local(node);
    }
    ok = 1;

cleanup:
    if (default_material)
        model_release_local(default_material);
    model_obj_material_table_free(&materials);
    rt_mesh3d_obj_groups_free(groups, group_count);
    if (temp_path) {
        rt_io_file_delete(temp_path);
        rt_string_unref(temp_path);
    }
    return ok;
}

/// @brief .gltf/.glb branch of rt_model3d_load_impl: decode the asset and collect its
///   meshes/materials/skeletons/animations/cameras/scene templates onto `model`.
///   Consumes the preloaded gltf inputs. @return 1 on success, 0 on failure (the local
///   asset is released; the caller cleans up `model`).
static int model3d_load_from_gltf(rt_model3d *model,
                                  rt_string path,
                                  int load_assets,
                                  uint8_t *preloaded_gltf_data,
                                  size_t preloaded_gltf_size,
                                  struct rt_gltf_preload_bundle *preloaded_gltf_bundle) {
    void *asset = NULL;
    int32_t mesh_count;
    int32_t material_count;
    int32_t skeleton_count;
    int32_t animation_count;
    int32_t node_animation_count;
    int32_t camera_count;
    int32_t gltf_scene_count;
    void *scene_root;
    if (preloaded_gltf_bundle) {
        asset = rt_gltf_load_preloaded_bundle(path, preloaded_gltf_bundle, load_assets);
        preloaded_gltf_bundle = NULL;
    } else if (preloaded_gltf_data) {
        asset = rt_gltf_load_preloaded(path, preloaded_gltf_data, preloaded_gltf_size, load_assets);
        preloaded_gltf_data = NULL;
    } else {
        asset = load_assets ? rt_gltf_load_asset(path) : rt_gltf_load(path);
    }
    if (!asset)
        return 0;
    if (!model_count_i64_to_i32(
            rt_gltf_mesh_count(asset), &mesh_count, "Model3D.Load: too many glTF meshes") ||
        !model_count_i64_to_i32(rt_gltf_material_count(asset),
                                &material_count,
                                "Model3D.Load: too many glTF materials") ||
        !model_count_i64_to_i32(rt_gltf_skeleton_count(asset),
                                &skeleton_count,
                                "Model3D.Load: too many glTF skeletons") ||
        !model_count_i64_to_i32(rt_gltf_animation_count(asset),
                                &animation_count,
                                "Model3D.Load: too many glTF animations") ||
        !model_count_i64_to_i32(rt_gltf_node_animation_count(asset),
                                &node_animation_count,
                                "Model3D.Load: too many glTF node animations") ||
        !model_count_i64_to_i32(
            rt_gltf_camera_count(asset), &camera_count, "Model3D.Load: too many glTF cameras") ||
        !model_count_i64_to_i32(
            rt_gltf_scene_count(asset), &gltf_scene_count, "Model3D.Load: too many glTF scenes")) {
        model_release_local(asset);
        return 0;
    }
    for (int32_t i = 0; i < mesh_count; i++) {
        if (!model_append_ref(&model->meshes,
                              &model->mesh_count,
                              &model->mesh_capacity,
                              rt_gltf_get_mesh(asset, i),
                              "Model3D.Load: mesh list allocation failed")) {
            model_release_local(asset);
            return 0;
        }
    }
    for (int32_t i = 0; i < material_count; i++) {
        if (!model_append_ref(&model->materials,
                              &model->material_count,
                              &model->material_capacity,
                              rt_gltf_get_material(asset, i),
                              "Model3D.Load: material list allocation failed")) {
            model_release_local(asset);
            return 0;
        }
    }
    for (int32_t i = 0; i < skeleton_count; i++) {
        if (!model_append_ref(&model->skeletons,
                              &model->skeleton_count,
                              &model->skeleton_capacity,
                              rt_gltf_get_skeleton(asset, i),
                              "Model3D.Load: skeleton list allocation failed")) {
            model_release_local(asset);
            return 0;
        }
    }
    for (int32_t i = 0; i < animation_count; i++) {
        if (!model_append_ref(&model->animations,
                              &model->animation_count,
                              &model->animation_capacity,
                              rt_gltf_get_animation(asset, i),
                              "Model3D.Load: animation list allocation failed")) {
            model_release_local(asset);
            return 0;
        }
    }
    for (int32_t i = 0; i < node_animation_count; i++) {
        if (!model_append_ref(&model->node_animations,
                              &model->node_animation_count,
                              &model->node_animation_capacity,
                              rt_gltf_get_node_animation(asset, i),
                              "Model3D.Load: node animation list allocation failed")) {
            model_release_local(asset);
            return 0;
        }
    }
    for (int32_t i = 0; i < camera_count; i++) {
        if (!model_append_ref(&model->cameras,
                              &model->camera_count,
                              &model->camera_capacity,
                              rt_gltf_get_camera(asset, i),
                              "Model3D.Load: camera list allocation failed")) {
            model_release_local(asset);
            return 0;
        }
    }
    {
        int32_t variant_count = 0;
        if (!model_count_i64_to_i32(rt_gltf_variant_count(asset),
                                    &variant_count,
                                    "Model3D.Load: too many glTF material variants")) {
            model_release_local(asset);
            return 0;
        }
        if (variant_count > 0) {
            model->variant_names = (char **)calloc((size_t)variant_count, sizeof(char *));
            if (!model->variant_names) {
                rt_trap("Model3D.Load: variant name list allocation failed");
                model_release_local(asset);
                return 0;
            }
            /* Set the count before filling so the finalizer covers partial fills. */
            model->variant_count = variant_count;
            for (int32_t i = 0; i < variant_count; i++) {
                rt_string name = rt_gltf_get_variant_name(asset, i);
                model->variant_names[i] =
                    model_strdup_or(name ? rt_string_cstr(name) : NULL, "variant");
                if (!model->variant_names[i]) {
                    model_release_local(asset);
                    return 0;
                }
            }
        }
    }
    scene_root = rt_gltf_get_scene_root(asset);
    if (scene_root) {
        if (!model_clone_children_to_root(model->template_root,
                                          (const rt_scene_node3d *)scene_root)) {
            model_release_local(asset);
            return 0;
        }
        if (!model_collect_template_refs(model)) {
            model_release_local(asset);
            return 0;
        }
    }
    if (!scene_root && !model_build_synth_mesh_nodes(model)) {
        model_release_local(asset);
        return 0;
    }
    if (gltf_scene_count > 0) {
        for (int32_t si = 0; si < gltf_scene_count; si++) {
            rt_string scene_name = rt_gltf_get_scene_name(asset, si);
            const char *scene_name_cstr = scene_name ? rt_string_cstr(scene_name) : NULL;
            rt_scene_node3d *scene_template = NULL;
            int32_t model_scene_index = -1;
            int32_t scene_camera_count = 0;
            if (si == 0) {
                scene_template = model->template_root;
            } else {
                void *gltf_scene_root = rt_gltf_get_scene_root_at(asset, si);
                scene_template = (rt_scene_node3d *)rt_scene_node3d_new();
                if (!gltf_scene_root || !scene_template) {
                    if (scene_template)
                        model_release_local(scene_template);
                    model_release_local(asset);
                    return 0;
                }
                if (!model_clone_children_to_root(scene_template,
                                                  (const rt_scene_node3d *)gltf_scene_root)) {
                    model_release_local(scene_template);
                    model_release_local(asset);
                    return 0;
                }
            }
            if (!model_append_scene_entry(
                    model, scene_name_cstr, scene_template, &model_scene_index)) {
                if (si != 0)
                    model_release_local(scene_template);
                model_release_local(asset);
                return 0;
            }
            if (!model_count_i64_to_i32(rt_gltf_scene_camera_count(asset, si),
                                        &scene_camera_count,
                                        "Model3D.Load: too many glTF scene cameras")) {
                if (si != 0)
                    model_release_local(scene_template);
                model_release_local(asset);
                return 0;
            }
            for (int32_t ci = 0; ci < scene_camera_count; ci++) {
                if (!model_append_scene_camera(&model->scenes[model_scene_index],
                                               rt_gltf_get_scene_camera(asset, si, ci))) {
                    if (si != 0)
                        model_release_local(scene_template);
                    model_release_local(asset);
                    return 0;
                }
            }
            if (si != 0)
                model_release_local(scene_template);
        }
    }
    model_release_local(asset);
    return 1;
}

/// @brief Write embedded FBX bytes directly to a temporary path.
///
/// The previous bridge wrapped @p bytes in a runtime Bytes object before writing,
/// doubling peak memory for large embedded FBX assets. This helper streams the
/// caller-owned byte span straight to the tempfile.
/// @param path Runtime string naming the temporary output file.
/// @param bytes Embedded asset byte span to write.
/// @param size Number of bytes in @p bytes.
/// @return Non-zero when the entire byte span was written and the file closed cleanly.
static int model3d_write_temp_asset_bytes(rt_string path, const uint8_t *bytes, size_t size) {
    const char *cpath;
    FILE *f;
    size_t written;

    if (!path || !bytes || size == 0)
        return 0;
    cpath = rt_string_cstr(path);
    if (!cpath)
        return 0;
    f = rt_file_stdio_open_utf8(cpath, "wb");
    if (!f)
        return 0;
    written = fwrite(bytes, 1, size, f);
    if (fclose(f) != 0 || written != size) {
        return 0;
    }
    return 1;
}

/// @brief Stage asset bytes in a uniquely named temporary file with the requested extension.
/// @return Owned runtime path on success, or NULL after cleaning a partial file on failure.
static rt_string model3d_spill_asset_bytes(const char *prefix_cstr,
                                           const char *suffix_cstr,
                                           const uint8_t *bytes,
                                           size_t size) {
    rt_string prefix;
    rt_string suffix;
    rt_string path;
    if (!prefix_cstr || !suffix_cstr || !bytes || size == 0)
        return NULL;
    prefix = rt_const_cstr(prefix_cstr);
    suffix = rt_const_cstr(suffix_cstr);
    path = rt_tempfile_path_with_ext(prefix, suffix);
    rt_string_unref(prefix);
    rt_string_unref(suffix);
    if (!path)
        return NULL;
    if (!model3d_write_temp_asset_bytes(path, bytes, size)) {
        rt_io_file_delete(path);
        rt_string_unref(path);
        return NULL;
    }
    return path;
}

/// @brief Decode a binary/ASCII FBX from in-memory asset bytes via a temp-file bridge.
/// @details The FBX importer is path-based (binary parsing re-opens the file), so embedded or
///   packed assets are spilled to a temp file, decoded with the recoverable loader, and
///   unlinked — mirroring the asset-decode tempfile bridge used for path-only image formats.
/// @return FBX asset handle on success, or NULL on any failure.
static void *model3d_fbx_from_asset_bytes(rt_string original_path,
                                          const uint8_t *bytes,
                                          size_t size) {
    if (!bytes || size == 0)
        return NULL;
    rt_string prefix = rt_const_cstr("zanna_fbx_");
    rt_string suffix = rt_const_cstr(".fbx");
    rt_string tmp = rt_tempfile_path_with_ext(prefix, suffix);
    rt_string_unref(prefix);
    rt_string_unref(suffix);
    if (!tmp)
        return NULL;
    const char *tpath = rt_string_cstr(tmp);
    if (!tpath) {
        rt_string_unref(tmp);
        return NULL;
    }
    (void)tpath;
    if (size > (size_t)INT64_MAX) {
        rt_string_unref(tmp);
        return NULL;
    }
    if (!model3d_write_temp_asset_bytes(tmp, bytes, size)) {
        rt_io_file_delete(tmp);
        rt_string_unref(tmp);
        return NULL;
    }
    void *asset = rt_fbx_load_recoverable_with_texture_base(tmp, original_path);
    rt_io_file_delete(tmp);
    rt_string_unref(tmp);
    return asset;
}

/// @brief .fbx branch of rt_model3d_load_impl: decode the asset and collect its meshes/
///   materials/skeleton/animations/scene template onto `model`.
/// @details Uses the recoverable FBX loader so missing or unsupported assets return 0 and let
///   `Model3D.Load` return NULL. When @p load_assets is set, embedded/packed bytes are tried
///   first (via a temp-file bridge) so `Model3D.LoadAsset` resolves bundled FBX models, with the
///   filesystem path as the fallback. Valid binary FBX files still use the strict parser,
///   preserving existing hard diagnostics for malformed binary content.
/// @return 1 on success, 0 on recoverable load failure.
static int model3d_load_from_fbx(rt_model3d *model,
                                 rt_string path,
                                 int load_assets,
                                 const uint8_t *preloaded_fbx_data,
                                 size_t preloaded_fbx_size) {
    void *asset = NULL;
    if (preloaded_fbx_data && preloaded_fbx_size > 0u)
        asset = model3d_fbx_from_asset_bytes(path, preloaded_fbx_data, preloaded_fbx_size);
    if (!asset && load_assets) {
        size_t asset_size = 0;
        uint8_t *asset_bytes = rt_asset_load_raw(path, &asset_size);
        if (asset_bytes) {
            asset = model3d_fbx_from_asset_bytes(path, asset_bytes, asset_size);
            free(asset_bytes);
        }
    }
    if (!asset)
        asset = rt_fbx_load_recoverable(path);
    int32_t mesh_count;
    int32_t material_count;
    int32_t animation_count;
    int32_t node_animation_count;
    int32_t camera_count;
    void *skeleton;
    void *scene_root;
    if (!asset)
        return 0;
    if (!model_count_i64_to_i32(
            rt_fbx_mesh_count(asset), &mesh_count, "Model3D.Load: too many FBX meshes") ||
        !model_count_i64_to_i32(rt_fbx_material_count(asset),
                                &material_count,
                                "Model3D.Load: too many FBX materials") ||
        !model_count_i64_to_i32(rt_fbx_animation_count(asset),
                                &animation_count,
                                "Model3D.Load: too many FBX animations") ||
        !model_count_i64_to_i32(rt_fbx_node_animation_count(asset),
                                &node_animation_count,
                                "Model3D.Load: too many FBX node animations") ||
        !model_count_i64_to_i32(
            rt_fbx_camera_count(asset), &camera_count, "Model3D.Load: too many FBX cameras")) {
        model_release_local(asset);
        return 0;
    }
    skeleton = rt_fbx_get_skeleton(asset);
    scene_root = rt_fbx_get_scene_root(asset);

    for (int32_t i = 0; i < mesh_count; i++) {
        void *mesh = rt_fbx_get_mesh(asset, i);
        void *morph_targets = rt_fbx_get_morph_target(asset, i);
        if (morph_targets)
            rt_mesh3d_set_morph_targets(mesh, morph_targets);
        if (!model_append_ref(&model->meshes,
                              &model->mesh_count,
                              &model->mesh_capacity,
                              mesh,
                              "Model3D.Load: mesh list allocation failed")) {
            model_release_local(asset);
            return 0;
        }
    }
    for (int32_t i = 0; i < material_count; i++) {
        if (!model_append_ref(&model->materials,
                              &model->material_count,
                              &model->material_capacity,
                              rt_fbx_get_material(asset, i),
                              "Model3D.Load: material list allocation failed")) {
            model_release_local(asset);
            return 0;
        }
    }
    if (skeleton) {
        if (!model_append_ref(&model->skeletons,
                              &model->skeleton_count,
                              &model->skeleton_capacity,
                              skeleton,
                              "Model3D.Load: skeleton list allocation failed")) {
            model_release_local(asset);
            return 0;
        }
    }
    for (int32_t i = 0; i < animation_count; i++) {
        if (!model_append_ref(&model->animations,
                              &model->animation_count,
                              &model->animation_capacity,
                              rt_fbx_get_animation(asset, i),
                              "Model3D.Load: animation list allocation failed")) {
            model_release_local(asset);
            return 0;
        }
    }
    for (int32_t i = 0; i < node_animation_count; ++i) {
        if (!model_append_ref(&model->node_animations,
                              &model->node_animation_count,
                              &model->node_animation_capacity,
                              rt_fbx_get_node_animation(asset, i),
                              "Model3D.Load: node animation list allocation failed")) {
            model_release_local(asset);
            return 0;
        }
    }
    for (int32_t i = 0; i < camera_count; ++i) {
        if (!model_append_ref(&model->cameras,
                              &model->camera_count,
                              &model->camera_capacity,
                              rt_fbx_get_camera(asset, i),
                              "Model3D.Load: camera list allocation failed")) {
            model_release_local(asset);
            return 0;
        }
    }
    if (scene_root) {
        if (!model_clone_children_to_root(model->template_root,
                                          (const rt_scene_node3d *)scene_root)) {
            model_release_local(asset);
            return 0;
        }
        if (!model_collect_template_refs(model)) {
            model_release_local(asset);
            return 0;
        }
        {
            int32_t scene_index = -1;
            if (!model_append_scene_entry(model, "default", model->template_root, &scene_index)) {
                model_release_local(asset);
                return 0;
            }
            for (int32_t i = 0; i < camera_count; ++i) {
                if (!model_append_scene_camera(&model->scenes[scene_index],
                                               rt_fbx_get_camera(asset, i))) {
                    model_release_local(asset);
                    return 0;
                }
            }
        }
    }
    if (!model_build_synth_mesh_nodes(model)) {
        model_release_local(asset);
        return 0;
    }
    model_release_local(asset);
    return 1;
}

/// @brief Load a 3D model from disk into a `rt_model3d` template — auto-detects format by file
/// extension (.vscn, .gltf/.glb, .fbx, .obj, .stl). Collects meshes, materials, skeletons,
/// animations, and the scene-node template tree from the source asset, retaining each component for
/// the model's lifetime. Returns NULL for recoverable loader failure; invalid paths and unsupported
/// extensions still trap. Use `_instantiate` to spawn a clone in a live scene.
static void *rt_model3d_load_impl(rt_string path,
                                  int load_assets,
                                  uint8_t *preloaded_gltf_data,
                                  size_t preloaded_gltf_size,
                                  struct rt_gltf_preload_bundle *preloaded_gltf_bundle,
                                  uint8_t *preloaded_fbx_data,
                                  size_t preloaded_fbx_size) {
    const char *path_cstr = path ? rt_string_cstr(path) : NULL;
    const char *api_name = load_assets ? "Model3D.LoadAsset" : "Model3D.Load";
    rt_model3d *model;

    if (!path || !path_cstr) {
        free(preloaded_gltf_data);
        free(preloaded_fbx_data);
        rt_gltf_preload_bundle_free(preloaded_gltf_bundle);
        rt_trap(load_assets ? "Model3D.LoadAsset: invalid path" : "Model3D.Load: invalid path");
        return NULL;
    }

    model = model_new();
    if (!model) {
        free(preloaded_gltf_data);
        free(preloaded_fbx_data);
        rt_gltf_preload_bundle_free(preloaded_gltf_bundle);
        return NULL;
    }
    model_set_root_name(model->template_root, path_cstr);

    if (model_has_ext(path_cstr, ".scene3d") || model_has_ext(path_cstr, ".vscn")) {
        rt_scene3d *scene = NULL;
        if (load_assets) {
            size_t asset_size = 0;
            uint8_t *asset_bytes = rt_asset_load_raw(path, &asset_size);
            if (asset_bytes) {
                scene = (rt_scene3d *)rt_scene3d_load_from_memory(
                    path, (const char *)asset_bytes, asset_size);
                free(asset_bytes);
                if (!scene)
                    goto fail;
            }
        }
        if (!scene)
            scene = (rt_scene3d *)rt_scene3d_load(path);
        if (!scene)
            goto fail;
        if (scene->baked_asset) {
            rt_vscn_loaded_asset3d *asset = scene->baked_asset;
            if (!asset->scenes || asset->scene_count <= 0)
                goto vscn_fail;
            for (int32_t i = 0; i < asset->mesh_count; ++i) {
                if (!model_append_ref(&model->meshes,
                                      &model->mesh_count,
                                      &model->mesh_capacity,
                                      asset->meshes[i],
                                      "Model3D.Load: VSCN mesh list allocation failed"))
                    goto vscn_fail;
            }
            for (int32_t i = 0; i < asset->material_count; ++i) {
                if (!model_append_ref(&model->materials,
                                      &model->material_count,
                                      &model->material_capacity,
                                      asset->materials[i],
                                      "Model3D.Load: VSCN material list allocation failed"))
                    goto vscn_fail;
            }
            for (int32_t i = 0; i < asset->skeleton_count; ++i) {
                if (!model_append_ref(&model->skeletons,
                                      &model->skeleton_count,
                                      &model->skeleton_capacity,
                                      asset->skeletons[i],
                                      "Model3D.Load: VSCN skeleton list allocation failed"))
                    goto vscn_fail;
            }
            for (int32_t i = 0; i < asset->animation_count; ++i) {
                if (!model_append_ref(&model->animations,
                                      &model->animation_count,
                                      &model->animation_capacity,
                                      asset->animations[i],
                                      "Model3D.Load: VSCN animation list allocation failed"))
                    goto vscn_fail;
            }
            for (int32_t i = 0; i < asset->node_animation_count; ++i) {
                if (!model_append_ref(&model->node_animations,
                                      &model->node_animation_count,
                                      &model->node_animation_capacity,
                                      asset->node_animations[i],
                                      "Model3D.Load: VSCN node-animation list allocation failed"))
                    goto vscn_fail;
            }
            for (int32_t i = 0; i < asset->camera_count; ++i) {
                if (!model_append_ref(&model->cameras,
                                      &model->camera_count,
                                      &model->camera_capacity,
                                      asset->cameras[i],
                                      "Model3D.Load: VSCN camera list allocation failed"))
                    goto vscn_fail;
            }
            if (asset->variant_count > 0) {
                model->variant_names =
                    (char **)calloc((size_t)asset->variant_count, sizeof(char *));
                if (!model->variant_names)
                    goto vscn_fail;
                model->variant_count = asset->variant_count;
                for (int32_t i = 0; i < asset->variant_count; ++i) {
                    model->variant_names[i] = model_strdup_or(asset->variant_names[i], "variant");
                    if (!model->variant_names[i])
                        goto vscn_fail;
                }
            }
            if (!model_clone_children_to_root(model->template_root, asset->scenes[0].root) ||
                !model_collect_template_refs(model))
                goto vscn_fail;
            for (int32_t scene_index = 0; scene_index < asset->scene_count; ++scene_index) {
                rt_scene_node3d *scene_template = model->template_root;
                int32_t model_scene_index = -1;
                if (scene_index > 0) {
                    scene_template = (rt_scene_node3d *)rt_scene_node3d_new();
                    if (!scene_template || !model_clone_children_to_root(
                                               scene_template, asset->scenes[scene_index].root)) {
                        model_release_local(scene_template);
                        goto vscn_fail;
                    }
                }
                if (!model_append_scene_entry(model,
                                              asset->scenes[scene_index].name,
                                              scene_template,
                                              &model_scene_index)) {
                    if (scene_index > 0)
                        model_release_local(scene_template);
                    goto vscn_fail;
                }
                for (int32_t camera_index = 0;
                     camera_index < asset->scenes[scene_index].camera_count;
                     ++camera_index) {
                    int32_t table_index = asset->scenes[scene_index].camera_indices[camera_index];
                    if (table_index < 0 || table_index >= asset->camera_count ||
                        !model_append_scene_camera(&model->scenes[model_scene_index],
                                                   asset->cameras[table_index])) {
                        if (scene_index > 0)
                            model_release_local(scene_template);
                        goto vscn_fail;
                    }
                }
                if (scene_index > 0)
                    model_release_local(scene_template);
            }
        } else {
            if (!model_clone_children_to_root(model->template_root, scene->root) ||
                !model_collect_template_refs(model))
                goto vscn_fail;
            /* v3 baked files carry animation clips on the scene; the SceneAsset owns
             * them from here (the scene's slots are cleared so its release skips them). */
            if (scene->baked_animation_count > 0 && scene->baked_animations &&
                model->animation_count == 0) {
                free(model->animations);
                model->animations = scene->baked_animations;
                model->animation_count = scene->baked_animation_count;
                model->animation_capacity = scene->baked_animation_count;
                scene->baked_animations = NULL;
                scene->baked_animation_count = 0;
            }
        }
        model_release_local(scene);
        scene = NULL;
        goto vscn_loaded;
    vscn_fail:
        model_release_local(scene);
        goto fail;
    vscn_loaded:;
    } else if (model_has_ext(path_cstr, ".gltf") || model_has_ext(path_cstr, ".glb")) {
        int gltf_ok = model3d_load_from_gltf(model,
                                             path,
                                             load_assets,
                                             preloaded_gltf_data,
                                             preloaded_gltf_size,
                                             preloaded_gltf_bundle);
        preloaded_gltf_data = NULL;
        preloaded_gltf_bundle = NULL;
        if (!gltf_ok)
            goto fail;
    } else if (model_has_ext(path_cstr, ".fbx")) {
        int fbx_ok =
            model3d_load_from_fbx(model, path, load_assets, preloaded_fbx_data, preloaded_fbx_size);
        free(preloaded_fbx_data);
        preloaded_fbx_data = NULL;
        preloaded_fbx_size = 0u;
        if (!fbx_ok)
            goto fail;
    } else if (model_has_ext(path_cstr, ".obj")) {
        if (!model3d_load_from_obj(model, path, path_cstr, load_assets))
            goto fail;
    } else if (model_has_ext(path_cstr, ".stl")) {
        void *mesh = NULL;
        void *material = NULL;
        if (load_assets) {
            size_t asset_size = 0;
            uint8_t *asset_bytes = rt_asset_load_raw(path, &asset_size);
            if (asset_bytes) {
                rt_string temp_path =
                    model3d_spill_asset_bytes("zanna_stl_", ".stl", asset_bytes, asset_size);
                free(asset_bytes);
                if (!temp_path) {
                    rt_asset_error_set_if_empty(RT_ASSET_ERROR_UNREADABLE,
                                                "Model3D.LoadAsset: failed to stage STL bytes");
                    goto fail;
                }
                mesh = rt_mesh3d_from_stl(temp_path);
                rt_io_file_delete(temp_path);
                rt_string_unref(temp_path);
                if (!mesh)
                    goto fail;
            }
        }
        if (!mesh)
            mesh = rt_mesh3d_from_stl(path);
        if (!mesh)
            goto fail;
        material = rt_material3d_new();
        if (!material) {
            model_release_local(mesh);
            goto fail;
        }
        if (!model_append_ref(&model->meshes,
                              &model->mesh_count,
                              &model->mesh_capacity,
                              mesh,
                              "Model3D.Load: mesh list allocation failed")) {
            model_release_local(material);
            model_release_local(mesh);
            goto fail;
        }
        if (!model_append_ref(&model->materials,
                              &model->material_count,
                              &model->material_capacity,
                              material,
                              "Model3D.Load: material list allocation failed")) {
            model_release_local(material);
            model_release_local(mesh);
            goto fail;
        }
        model_release_local(material);
        model_release_local(mesh);
        if (!model_build_synth_mesh_nodes(model))
            goto fail;
    } else {
        rt_asset_error_setf(RT_ASSET_ERROR_UNSUPPORTED, "%s: unsupported file extension", api_name);
        goto fail;
    }

    if (model->scene_count == 0 && model->template_root) {
        if (!model_append_scene_entry(model, "default", model->template_root, NULL))
            goto fail;
    }

    return model;

fail:
    free(preloaded_gltf_data);
    free(preloaded_fbx_data);
    rt_gltf_preload_bundle_free(preloaded_gltf_bundle);
    model_release_local(model);
    return NULL;
}
