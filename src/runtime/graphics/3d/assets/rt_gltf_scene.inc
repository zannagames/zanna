//===----------------------------------------------------------------------===//
// Part of the Zanna project, under the GNU GPL v3. See LICENSE.
// File: src/runtime/graphics/3d/assets/rt_gltf_scene.inc  (#included from rt_gltf.c; not a
// standalone translation unit) Purpose: glTF scene-graph node build and scene import
//===----------------------------------------------------------------------===//

/**
 * @file rt_gltf_scene.inc
 * @brief Builds runtime node templates and scene roots from a decoded glTF asset.
 * @details This `rt_gltf.c` fragment applies node transforms, lights, meshes, skins, primitive
 *          materials, and material variants; links the validated template hierarchy; clones
 *          scene-specific root subtrees; imports scene camera metadata; and installs the active
 *          scene compatibility view.
 */

/// @brief Promote a glTF node's application-defined `extras` into typed SceneNode metadata
///        (ADR 0294).
/// @details Strings, integers, finite floats, booleans, and JSON null map onto the matching
///          metadata kinds. Nested objects/arrays, oversized keys or strings, non-finite numbers,
///          and entries beyond the node metadata capacity are skipped and reported once per node
///          as an asset warning. A non-object `extras` value is ignored (the specification allows
///          any JSON value there).
/// @param node Runtime node receiving the metadata.
/// @param extras Borrowed parsed `extras` value, or NULL.
/// @param node_name Display name used in the warning.
static void gltf_node_import_extras(rt_scene_node3d *node, void *extras, const char *node_name) {
    void *keys;
    int64_t count;
    int32_t skipped = 0;
    if (!node || !extras || !rt_obj_is_instance(extras, RT_MAP_CLASS_ID, 0))
        return;
    if (rt_map_len(extras) <= 0)
        return;
    keys = rt_map_keys(extras);
    if (!keys)
        return;
    count = rt_seq_len(keys);
    for (int64_t i = 0; i < count; i++) {
        rt_string key = (rt_string)rt_seq_get(keys, i);
        const char *key_data;
        size_t key_len;
        void *value;
        int8_t ok = 0;
        if (!rt_string_is_handle(key)) {
            skipped++;
            continue;
        }
        key_data = rt_string_cstr(key);
        key_len = key_data ? strlen(key_data) : 0u;
        if (key_len == 0u || key_len > (size_t)RT_SCENE_NODE3D_MAX_METADATA_KEY_BYTES) {
            skipped++;
            continue;
        }
        value = rt_map_get(extras, key);
        if (!value) {
            ok = rt_scene_node3d_metadata_import_internal(
                node, key_data, (int32_t)key_len, RT_SCENE3D_METADATA_NULL, 0, 0, 0.0, NULL, 0);
        } else if (rt_string_is_handle(value)) {
            const char *text = rt_string_cstr((rt_string)value);
            size_t text_len = text ? strlen(text) : 0u;
            if (text && text_len <= (size_t)RT_SCENE_NODE3D_MAX_METADATA_STRING_BYTES)
                ok = rt_scene_node3d_metadata_import_internal(node,
                                                              key_data,
                                                              (int32_t)key_len,
                                                              RT_SCENE3D_METADATA_STRING,
                                                              0,
                                                              0,
                                                              0.0,
                                                              text,
                                                              (int32_t)text_len);
        } else {
            switch (rt_box_type(value)) {
                case 0:
                    ok = rt_scene_node3d_metadata_import_internal(node,
                                                                  key_data,
                                                                  (int32_t)key_len,
                                                                  RT_SCENE3D_METADATA_INT,
                                                                  0,
                                                                  rt_unbox_i64(value),
                                                                  0.0,
                                                                  NULL,
                                                                  0);
                    break;
                case 1: {
                    /* The JSON tree boxes every number as a double; an integral value
                     * (Blender int custom properties, counts, indices) imports as int so
                     * MetadataGetInt reads it back, fractional values stay float. */
                    double number = rt_unbox_f64(value);
                    int64_t integral;
                    if (!isfinite(number))
                        break;
                    if (gltf_double_to_i64_checked(number, &integral))
                        ok = rt_scene_node3d_metadata_import_internal(node,
                                                                      key_data,
                                                                      (int32_t)key_len,
                                                                      RT_SCENE3D_METADATA_INT,
                                                                      0,
                                                                      integral,
                                                                      0.0,
                                                                      NULL,
                                                                      0);
                    else
                        ok = rt_scene_node3d_metadata_import_internal(node,
                                                                      key_data,
                                                                      (int32_t)key_len,
                                                                      RT_SCENE3D_METADATA_FLOAT,
                                                                      0,
                                                                      0,
                                                                      number,
                                                                      NULL,
                                                                      0);
                    break;
                }
                case 2:
                    ok = rt_scene_node3d_metadata_import_internal(node,
                                                                  key_data,
                                                                  (int32_t)key_len,
                                                                  RT_SCENE3D_METADATA_BOOL,
                                                                  rt_unbox_i1(value) ? 1 : 0,
                                                                  0,
                                                                  0.0,
                                                                  NULL,
                                                                  0);
                    break;
                default:
                    break; /* nested object/array: opaque by design */
            }
        }
        if (!ok)
            skipped++;
    }
    if (rt_obj_release_check0(keys))
        rt_obj_free(keys);
    if (skipped > 0)
        rt_asset_error_add_warningf("glTF node '%s' extras: %d entr%s not imported (unsupported "
                                    "value kind or metadata limits)",
                                    node_name ? node_name : "",
                                    (int)skipped,
                                    skipped == 1 ? "y" : "ies");
}

/// @brief Apply a glTF node's matrix or component transform to a runtime scene node.
/// @details A 4x4 matrix takes precedence over translation, rotation, and scale arrays.
///          The result is decomposed as needed, sanitized, and marked world-dirty.
/// @param[in] node_json Parsed glTF node object.
/// @param[in,out] node Runtime node receiving local transform components.
static void gltf_node_read_transform(void *node_json, rt_scene_node3d *node) {
    void *matrix_arr = jarr(node_json, "matrix");
    if (matrix_arr && jarr_len(matrix_arr) >= 16) {
        double m[16];
        double row_major[16];
        for (int i = 0; i < 16; i++)
            m[i] = jvalue_num(rt_seq_get(matrix_arr, (int64_t)i), i % 5 == 0 ? 1.0 : 0.0);
        gltf_matrix_column_major_to_row_major(m, row_major);
        gltf_matrix_to_trs(row_major, node->position, node->rotation, node->scale_xyz);
    } else {
        void *translation = jarr(node_json, "translation");
        void *rotation = jarr(node_json, "rotation");
        void *scale_arr = jarr(node_json, "scale");
        node->position[0] = gltf_arr_num(translation, 0, 0.0);
        node->position[1] = gltf_arr_num(translation, 1, 0.0);
        node->position[2] = gltf_arr_num(translation, 2, 0.0);
        node->rotation[0] = gltf_arr_num(rotation, 0, 0.0);
        node->rotation[1] = gltf_arr_num(rotation, 1, 0.0);
        node->rotation[2] = gltf_arr_num(rotation, 2, 0.0);
        node->rotation[3] = gltf_arr_num(rotation, 3, 1.0);
        node->scale_xyz[0] = gltf_arr_num(scale_arr, 0, 1.0);
        node->scale_xyz[1] = gltf_arr_num(scale_arr, 1, 1.0);
        node->scale_xyz[2] = gltf_arr_num(scale_arr, 2, 1.0);
    }
    gltf_sanitize_trs(node->position, node->rotation, node->scale_xyz);
    node->world_dirty = 1;
}

/// @brief Bind a referenced imported punctual light to a runtime node.
/// @param[in,out] node Runtime node receiving the light.
/// @param light_ref glTF light index, or a negative value when no light is referenced.
/// @param[in] imported_lights Runtime light table indexed by glTF light index.
/// @param imported_light_count Number of entries in @p imported_lights.
/// @return Non-zero when no light is requested or a valid light is bound; zero for an
///         invalid or unavailable nonnegative reference.
static int gltf_node_bind_light(rt_scene_node3d *node,
                                int64_t light_ref,
                                void **imported_lights,
                                int32_t imported_light_count) {
    if (light_ref < 0)
        return 1;
    if (light_ref < imported_light_count && imported_lights && imported_lights[light_ref]) {
        rt_scene_node3d_set_light(node, imported_lights[light_ref]);
        return 1;
    }
    return 0;
}

/// @brief Assign the resolved default material for one output primitive mesh.
/// @param[in,out] node Runtime node receiving the material.
/// @param[in] primitive_materials Per-output-mesh resolved material table.
/// @param primitive_index Zero-based output primitive slot.
static void gltf_node_set_primitive_material(rt_scene_node3d *node,
                                             void **primitive_materials,
                                             int32_t primitive_index) {
    if (primitive_materials && primitive_index >= 0 && primitive_materials[primitive_index])
        rt_scene_node3d_set_material(node, primitive_materials[primitive_index]);
}

/// @brief Install the primitive's KHR_materials_variants table on @p node.
/// @details Resolves each variant's material index into the asset material list; variants the
///          primitive does not map keep the primitive's default material, so applying any
///          variant is always reversible. No-op when the asset has no variants or the
///          primitive carries no mappings.
/// @param[in] asset Asset providing variant count and resolved material objects.
/// @param[in,out] node Runtime primitive node receiving the variant material table.
/// @param[in] primitive_variant_mappings Per-output-mesh variant-to-material-index tables.
/// @param primitive_index Zero-based output primitive slot.
/// @param[in] primitive_materials Per-output-mesh default material table.
static void gltf_node_bind_variant_materials(rt_gltf_asset *asset,
                                             rt_scene_node3d *node,
                                             int32_t **primitive_variant_mappings,
                                             int32_t primitive_index,
                                             void **primitive_materials) {
    const int32_t *mapping;
    void **table;
    void *default_material;
    int32_t count;
    if (!asset || !node || !primitive_variant_mappings || primitive_index < 0)
        return;
    mapping = primitive_variant_mappings[primitive_index];
    count = asset->variant_name_count;
    if (!mapping || count <= 0)
        return;
    default_material = primitive_materials ? primitive_materials[primitive_index] : NULL;
    table = (void **)calloc((size_t)count, sizeof(void *));
    if (!table)
        return;
    for (int32_t v = 0; v < count; v++) {
        void *material = default_material;
        if (mapping[v] >= 0 && mapping[v] < asset->material_count && asset->materials)
            material = asset->materials[mapping[v]];
        table[v] = material;
    }
    rt_scene_node3d_assign_variant_materials(node, table, count);
    free(table);
}

/// @brief Create or select the mesh variant required by a node and attach it.
/// @details Applies node-level morph weights and skin remapping through
///          gltf_make_node_mesh_variant, releasing any temporary variant after the
///          node retains it.
/// @param[in] asset Asset owning the decoded primitive mesh table.
/// @param[in,out] node Runtime node receiving the mesh.
/// @param mesh_index Zero-based output primitive mesh slot.
/// @param skin_ref glTF skin index, or a negative value for an unskinned node.
/// @param[in] weights_arr Optional node-level morph weights array.
/// @param[in] skins Decoded skin table.
/// @param skin_count Number of entries in @p skins.
/// @return Non-zero when a valid mesh variant is attached; zero on invalid input or
///         variant creation failure.
static int gltf_node_attach_primitive_mesh(rt_gltf_asset *asset,
                                           rt_scene_node3d *node,
                                           int32_t mesh_index,
                                           int64_t skin_ref,
                                           void *weights_arr,
                                           const gltf_skin_t *skins,
                                           int32_t skin_count) {
    void *node_mesh;
    if (!asset || !node || mesh_index < 0 || mesh_index >= asset->mesh_count)
        return 0;
    node_mesh =
        gltf_make_node_mesh_variant(asset, mesh_index, skin_ref, weights_arr, skins, skin_count);
    if (!node_mesh)
        return 0;
    rt_scene_node3d_set_mesh(node, node_mesh);
    if (node_mesh != asset->meshes[mesh_index])
        gltf_release_local(node_mesh);
    return 1;
}

/// @brief Attach an additional source-mesh primitive as a child node.
/// @details Creates a child node, installs its mesh variant, default and variant
///          materials, attaches it to the source node, and releases the local reference.
/// @param[in] asset Asset owning meshes, materials, and variant metadata.
/// @param[in,out] node Parent runtime node representing the source glTF node.
/// @param mesh_index Zero-based output primitive mesh slot.
/// @param skin_ref glTF skin index, or a negative value for no skin.
/// @param[in] weights_arr Optional node-level morph weights array.
/// @param[in] primitive_materials Per-output-mesh default material table.
/// @param[in] primitive_variant_mappings Per-output-mesh variant mapping tables.
/// @param[in] skins Decoded skin table.
/// @param skin_count Number of entries in @p skins.
/// @return Non-zero when the child primitive is attached; zero on allocation,
///         mesh-variant, or hierarchy insertion failure.
static int gltf_node_attach_extra_primitive(rt_gltf_asset *asset,
                                            rt_scene_node3d *node,
                                            int32_t mesh_index,
                                            int64_t skin_ref,
                                            void *weights_arr,
                                            void **primitive_materials,
                                            int32_t **primitive_variant_mappings,
                                            const gltf_skin_t *skins,
                                            int32_t skin_count) {
    rt_scene_node3d *prim_node = (rt_scene_node3d *)rt_scene_node3d_new();
    if (!prim_node)
        return 0;
    if (!gltf_node_attach_primitive_mesh(
            asset, prim_node, mesh_index, skin_ref, weights_arr, skins, skin_count)) {
        gltf_release_local(prim_node);
        return 0;
    }
    gltf_node_set_primitive_material(prim_node, primitive_materials, mesh_index);
    gltf_node_bind_variant_materials(
        asset, prim_node, primitive_variant_mappings, mesh_index, primitive_materials);
    if (!rt_scene_node3d_try_add_child(node, prim_node)) {
        gltf_release_local(prim_node);
        return 0;
    }
    gltf_release_local(prim_node);
    return 1;
}

/// @brief Attach every runtime primitive produced for a referenced glTF mesh.
/// @details The first primitive is installed directly on @p node; remaining primitives
///          become child nodes so each can retain its own mesh and material state.
/// @param[in] asset Asset owning decoded meshes and materials.
/// @param[in,out] node Runtime node representing the source glTF node.
/// @param mesh_ref glTF mesh index, or a negative value for no mesh.
/// @param skin_ref glTF skin index, or a negative value for no skin.
/// @param[in] weights_arr Optional node-level morph weights array.
/// @param mesh_json_count Number of source glTF mesh objects.
/// @param[in] mesh_prim_start Per-source-mesh first output primitive slot table.
/// @param[in] mesh_prim_count Per-source-mesh emitted primitive count table.
/// @param[in] primitive_materials Per-output-mesh default material table.
/// @param[in] primitive_variant_mappings Per-output-mesh variant mapping tables.
/// @param[in] skins Decoded skin table.
/// @param skin_count Number of entries in @p skins.
/// @return Non-zero when no mesh is referenced or every primitive is attached; zero
///         for an invalid range or attachment failure.
static int gltf_node_attach_mesh_primitives(rt_gltf_asset *asset,
                                            rt_scene_node3d *node,
                                            int64_t mesh_ref,
                                            int64_t skin_ref,
                                            void *weights_arr,
                                            int32_t mesh_json_count,
                                            int *mesh_prim_start,
                                            int *mesh_prim_count,
                                            void **primitive_materials,
                                            int32_t **primitive_variant_mappings,
                                            const gltf_skin_t *skins,
                                            int32_t skin_count) {
    int32_t prim_start;
    int32_t prim_count;
    if (mesh_ref < 0)
        return 1;
    /* Reject an out-of-range mesh reference unconditionally — including when the
     * whole document declares zero primitives, in which case the prim tables are
     * NULL. This bounds check is what makes GLTF.Load reject a node pointing at a
     * non-existent mesh; gating it behind the prim-table NULL check (below) let
     * such nodes load silently. */
    if (mesh_ref >= mesh_json_count)
        return 0;
    if (!mesh_prim_count || !mesh_prim_start)
        return 1;
    prim_start = mesh_prim_start[mesh_ref];
    prim_count = mesh_prim_count[mesh_ref];
    if (prim_count <= 0)
        return 1;
    if (prim_start < 0 || prim_start > asset->mesh_count ||
        prim_count > asset->mesh_count - prim_start)
        return 0;
    if (!gltf_node_attach_primitive_mesh(
            asset, node, prim_start, skin_ref, weights_arr, skins, skin_count))
        return 0;
    gltf_node_set_primitive_material(node, primitive_materials, prim_start);
    gltf_node_bind_variant_materials(
        asset, node, primitive_variant_mappings, prim_start, primitive_materials);
    for (int32_t pi = 1; pi < prim_count; pi++) {
        if (!gltf_node_attach_extra_primitive(asset,
                                              node,
                                              prim_start + pi,
                                              skin_ref,
                                              weights_arr,
                                              primitive_materials,
                                              primitive_variant_mappings,
                                              skins,
                                              skin_count))
            return 0;
    }
    return 1;
}

/// @brief Construct and populate one reusable runtime node template.
/// @details Applies its effective name, validates references, binds any light, imports
///          its local transform, and attaches all meshes generated for its source mesh.
/// @param[in] asset Asset owning decoded runtime resources.
/// @param[in] nodes_arr Parsed glTF `nodes` array.
/// @param node_index Zero-based source node index.
/// @param mesh_json_count Number of source glTF mesh objects.
/// @param[in] mesh_prim_start Per-source-mesh first output primitive slot table.
/// @param[in] mesh_prim_count Per-source-mesh emitted primitive count table.
/// @param[in] primitive_materials Per-output-mesh default material table.
/// @param[in] primitive_variant_mappings Per-output-mesh variant mapping tables.
/// @param[in] skins Decoded skin table.
/// @param skin_count Number of entries in @p skins.
/// @param[in] imported_lights Runtime punctual-light table.
/// @param imported_light_count Number of entries in @p imported_lights.
/// @param[out] out_node Receives the allocated node even when later population fails,
///                      allowing caller cleanup.
/// @return Non-zero when the template is fully populated; zero on allocation or an
///         invalid/unresolvable resource reference.
static int gltf_build_template_node(rt_gltf_asset *asset,
                                    void *nodes_arr,
                                    int32_t node_index,
                                    int32_t mesh_json_count,
                                    int *mesh_prim_start,
                                    int *mesh_prim_count,
                                    void **primitive_materials,
                                    int32_t **primitive_variant_mappings,
                                    gltf_skin_t *skins,
                                    int32_t skin_count,
                                    void **imported_lights,
                                    int32_t imported_light_count,
                                    rt_scene_node3d **out_node) {
    void *node_json = rt_seq_get(nodes_arr, (int64_t)node_index);
    rt_scene_node3d *node = (rt_scene_node3d *)rt_scene_node3d_new();
    const char *name = jstr(node_json, "name");
    char fallback_name[64];
    void *weights_arr = jarr(node_json, "weights");
    void *node_extensions = jget(node_json, "extensions");
    void *node_punctual_light =
        node_extensions ? jget(node_extensions, "KHR_lights_punctual") : NULL;
    int64_t mesh_ref = jint(node_json, "mesh", -1);
    int64_t skin_ref = jint(node_json, "skin", -1);
    int64_t light_ref = jint(node_punctual_light, "light", -1);

    *out_node = node;
    if (!node)
        return 0;
    node->import_index = node_index;
    if (!name || name[0] == '\0')
        name =
            gltf_effective_node_name(nodes_arr, node_index, fallback_name, sizeof(fallback_name));
    gltf_set_node_name(node, name);
    gltf_node_import_extras(node, jget(node_json, "extras"), name);
    if (mesh_ref < -1 || skin_ref < -1 || light_ref < -1)
        return 0;
    if (!gltf_node_bind_light(node, light_ref, imported_lights, imported_light_count))
        return 0;
    gltf_node_read_transform(node_json, node);
    if (skin_ref >= 0 && skin_ref >= skin_count)
        return 0;
    return gltf_node_attach_mesh_primitives(asset,
                                            node,
                                            mesh_ref,
                                            skin_ref,
                                            weights_arr,
                                            mesh_json_count,
                                            mesh_prim_start,
                                            mesh_prim_count,
                                            primitive_materials,
                                            primitive_variant_mappings,
                                            skins,
                                            skin_count);
}

/// @brief Link child relationships among all constructed node templates.
/// @param[in] nodes_arr Parsed glTF `nodes` array.
/// @param node_json_count Number of source node objects.
/// @param[in,out] nodes Template node table indexed by glTF node index.
/// @return Non-zero when every valid referenced child is attached; zero when a
///         hierarchy insertion fails.
static int gltf_link_template_node_children(void *nodes_arr,
                                            int32_t node_json_count,
                                            rt_scene_node3d **nodes) {
    for (int32_t ni = 0; ni < node_json_count && nodes; ni++) {
        void *node_json = rt_seq_get(nodes_arr, (int64_t)ni);
        void *children = jarr(node_json, "children");
        for (int64_t ci = 0; ci < jarr_len(children); ci++) {
            int64_t child_idx = jvalue_int(rt_seq_get(children, ci), -1);
            if (child_idx >= 0 && child_idx < node_json_count && nodes[ni] && nodes[child_idx]) {
                if (!rt_scene_node3d_try_add_child(nodes[ni], nodes[child_idx]))
                    return 0;
            }
        }
    }
    return 1;
}

/// @brief Map import order to a source scene index with the active scene first.
/// @param order Zero-based output import order.
/// @param active_scene Source index selected by the root `scene` property.
/// @param active_valid Non-zero when @p active_scene lies within the scenes array.
/// @return Source scene index for @p order, preserving source order after moving a
///         valid active scene to slot zero.
static int64_t gltf_scene_order_index(int64_t order, int64_t active_scene, int active_valid) {
    if (!active_valid)
        return order;
    return order == 0 ? active_scene : (order <= active_scene ? order - 1 : order);
}

/// @brief Validate that a scene lists distinct, in-range root nodes.
/// @param[in] scene_nodes Parsed scene `nodes` array.
/// @param node_json_count Number of source glTF nodes.
/// @param[in] node_parent Validated parent-index table, or `NULL`.
/// @param[out] scene_seen Scratch bitmap used to reject duplicate roots.
/// @return Non-zero when every listed node is unique, in range, and parentless.
static int gltf_scene_roots_are_valid(void *scene_nodes,
                                      int32_t node_json_count,
                                      const int *node_parent,
                                      int *scene_seen) {
    for (int64_t i = 0; i < jarr_len(scene_nodes); i++) {
        int64_t node_idx = jvalue_int(rt_seq_get(scene_nodes, i), -1);
        if (node_idx < 0 || node_idx >= node_json_count || scene_seen[node_idx] ||
            (node_parent && node_parent[node_idx] >= 0))
            return 0;
        scene_seen[node_idx] = 1;
    }
    return 1;
}

/// @brief Clone one template subtree and attach it below a scene root.
/// @details Missing or out-of-range template entries are treated as an empty no-op;
///          successful clones also mark their source subtree active for scene metadata.
/// @param[in,out] scene_root Destination scene root.
/// @param[in] nodes_arr Parsed glTF `nodes` array.
/// @param node_json_count Number of source nodes.
/// @param[in] nodes Template node table.
/// @param node_idx Source root node index to clone.
/// @param[in,out] scene_active Per-source-node active bitmap updated for the subtree.
/// @return Non-zero on successful attachment or a harmless missing template; zero on
///         clone, hierarchy insertion, or active-subtree traversal failure.
static int gltf_clone_scene_root_into(rt_scene_node3d *scene_root,
                                      void *nodes_arr,
                                      int32_t node_json_count,
                                      rt_scene_node3d **nodes,
                                      int32_t node_idx,
                                      uint8_t *scene_active) {
    rt_scene_node3d *clone;
    if (node_idx < 0 || node_idx >= node_json_count || !nodes[node_idx])
        return 1;
    clone = gltf_clone_scene_node(nodes[node_idx]);
    if (!clone)
        return 0;
    if (!rt_scene_node3d_try_add_child(scene_root, clone)) {
        gltf_release_local(clone);
        return 0;
    }
    gltf_release_local(clone);
    return gltf_mark_active_node_subtree(nodes_arr, node_json_count, node_idx, scene_active);
}

/// @brief Import one declared glTF scene from its JSON object.
/// @details Validates root membership, clones each template subtree into a new named
///          scene root, appends it to the asset, and imports cameras active in that scene.
/// @param[in,out] asset Asset receiving the new scene record.
/// @param[in] root Parsed glTF document root used for camera import.
/// @param[in] nodes_arr Parsed glTF `nodes` array.
/// @param[in] nodes Reusable runtime template node table.
/// @param node_json_count Number of source nodes.
/// @param[in] node_parent Validated parent-index table.
/// @param[in] scene_json Parsed scene object.
/// @param scene_index Zero-based source scene index used in fallback naming.
/// @param order Zero-based import order; the active scene is imported first.
/// @param[in,out] built_any_io Flag set when a scene is successfully appended.
/// @return Non-zero when the scene and its camera metadata are imported; zero on
///         invalid roots or allocation, cloning, append, or camera-import failure.
static int gltf_import_scene_from_json(rt_gltf_asset *asset,
                                       void *root,
                                       void *nodes_arr,
                                       rt_scene_node3d **nodes,
                                       int32_t node_json_count,
                                       const int *node_parent,
                                       void *scene_json,
                                       int64_t scene_index,
                                       int64_t order,
                                       int *built_any_io) {
    void *scene_nodes = jarr(scene_json, "nodes");
    int *scene_seen = (int *)calloc((size_t)node_json_count, sizeof(int));
    uint8_t *scene_active = (uint8_t *)calloc((size_t)node_json_count, sizeof(*scene_active));
    rt_scene_node3d *scene_root = NULL;
    int ok = scene_seen != NULL && scene_active != NULL;
    char fallback_scene_name[64];
    const char *scene_name = jstr(scene_json, "name");
    if (ok)
        ok = gltf_scene_roots_are_valid(scene_nodes, node_json_count, node_parent, scene_seen);
    if (ok) {
        scene_root = (rt_scene_node3d *)rt_scene_node3d_new();
        ok = scene_root != NULL;
    }
    for (int64_t i = 0; ok && i < jarr_len(scene_nodes); i++) {
        int64_t node_idx = jvalue_int(rt_seq_get(scene_nodes, i), -1);
        ok = gltf_clone_scene_root_into(
            scene_root, nodes_arr, node_json_count, nodes, (int32_t)node_idx, scene_active);
    }
    if (ok && scene_root) {
        snprintf(fallback_scene_name,
                 sizeof(fallback_scene_name),
                 order == 0 ? "default" : "scene_%d",
                 (int)scene_index);
        ok = gltf_append_scene(asset,
                               scene_root,
                               (scene_name && scene_name[0] != '\0') ? scene_name
                                                                     : fallback_scene_name);
        if (ok) {
            gltf_scene_info_t *scene = &asset->scenes[asset->scene_count - 1];
            ok = gltf_import_scene_cameras(scene, root, nodes_arr, node_json_count, scene_active);
            scene_root = NULL;
            *built_any_io = 1;
        }
    }
    if (scene_root)
        gltf_release_local(scene_root);
    free(scene_active);
    free(scene_seen);
    return ok;
}

/// @brief Synthesize a default scene from all parentless template nodes.
/// @param[in,out] asset Asset receiving the fallback scene.
/// @param[in] root Parsed glTF document root used for camera import.
/// @param[in] nodes_arr Parsed glTF `nodes` array.
/// @param[in] nodes Reusable runtime template node table.
/// @param node_json_count Number of source nodes.
/// @param[in] node_parent Validated parent-index table.
/// @return Non-zero when at least one root is cloned, appended, and has its cameras
///         imported; zero when no root exists or any required operation fails.
static int gltf_import_fallback_scene(rt_gltf_asset *asset,
                                      void *root,
                                      void *nodes_arr,
                                      rt_scene_node3d **nodes,
                                      int32_t node_json_count,
                                      const int *node_parent) {
    rt_scene_node3d *scene_root = (rt_scene_node3d *)rt_scene_node3d_new();
    uint8_t *scene_active = (uint8_t *)calloc((size_t)node_json_count, sizeof(*scene_active));
    int attached_any = 0;
    int ok = scene_root != NULL && scene_active != NULL;
    for (int32_t i = 0; ok && i < node_json_count; i++) {
        if (node_parent && node_parent[i] < 0 && nodes[i]) {
            ok = gltf_clone_scene_root_into(
                scene_root, nodes_arr, node_json_count, nodes, i, scene_active);
            if (ok)
                attached_any = 1;
        }
    }
    if (ok && attached_any && gltf_append_scene(asset, scene_root, "default")) {
        gltf_scene_info_t *scene = &asset->scenes[asset->scene_count - 1];
        ok = gltf_import_scene_cameras(scene, root, nodes_arr, node_json_count, scene_active);
        scene_root = NULL;
    } else if (ok) {
        ok = 0;
    }
    if (scene_root)
        gltf_release_local(scene_root);
    free(scene_active);
    return ok;
}

/// @brief Import declared scenes, synthesize a fallback if needed, and install compatibility state.
/// @details A valid active scene is imported first so asset scene slot zero represents
///          the selected scene while remaining scenes retain their relative source order.
/// @param[in,out] asset Asset receiving scene records and active-scene compatibility data.
/// @param[in] root Parsed glTF document root.
/// @param[in] nodes_arr Parsed glTF `nodes` array.
/// @param[in] nodes Reusable runtime template node table.
/// @param node_json_count Number of source nodes.
/// @param[in] node_parent Validated parent-index table.
/// @return Non-zero when all declared or fallback scenes and compatibility state are
///         installed; zero on import failure.
static int gltf_import_node_scenes(rt_gltf_asset *asset,
                                   void *root,
                                   void *nodes_arr,
                                   rt_scene_node3d **nodes,
                                   int32_t node_json_count,
                                   const int *node_parent) {
    void *scenes_arr = jarr(root, "scenes");
    int64_t active_scene = jint(root, "scene", 0);
    int64_t scene_count = jarr_len(scenes_arr);
    int built_any = 0;
    int active_valid =
        scenes_arr && scene_count > 0 && active_scene >= 0 && active_scene < scene_count;
    if (scenes_arr && scene_count > 0) {
        for (int64_t order = 0; order < scene_count; order++) {
            int64_t scene_index = gltf_scene_order_index(order, active_scene, active_valid);
            if (!gltf_import_scene_from_json(asset,
                                             root,
                                             nodes_arr,
                                             nodes,
                                             node_json_count,
                                             node_parent,
                                             rt_seq_get(scenes_arr, scene_index),
                                             scene_index,
                                             order,
                                             &built_any))
                return 0;
        }
    }
    if (!built_any &&
        !gltf_import_fallback_scene(asset, root, nodes_arr, nodes, node_json_count, node_parent))
        return 0;
    return gltf_install_active_scene_compat(asset);
}

/// @brief Build the reusable template node hierarchy and per-scene roots for a
///        glTF asset. Extracted post-asset phase of rt_gltf_load_impl; operates
///        only on already-decoded meshes/materials/skins and never early-returns.
/// @param[in,out] asset Asset receiving node-bound scenes and compatibility state.
/// @param[in] root Parsed glTF document root.
/// @param[in] mesh_prim_start Per-source-mesh first output primitive slot table.
/// @param[in] mesh_prim_count Per-source-mesh emitted primitive count table.
/// @param[in] primitive_materials Per-output-mesh default material table.
/// @param[in] primitive_variant_mappings Per-output-mesh variant mapping tables.
/// @param[in] skins Decoded skin table.
/// @param skin_count Number of entries in @p skins.
/// @param[in] imported_lights Runtime punctual-light table.
/// @param imported_light_count Number of entries in @p imported_lights.
/// @return Non-zero (load_failed) on a fatal allocation/clone failure.
static int gltf_build_node_hierarchy(rt_gltf_asset *asset,
                                     void *root,
                                     int *mesh_prim_start,
                                     int *mesh_prim_count,
                                     void **primitive_materials,
                                     int32_t **primitive_variant_mappings,
                                     gltf_skin_t *skins,
                                     int32_t skin_count,
                                     void **imported_lights,
                                     int32_t imported_light_count) {
    int load_failed = 0;
    int64_t mesh_json_count64 = jarr_len(jarr(root, "meshes"));
    if (mesh_json_count64 < 0 || mesh_json_count64 > INT32_MAX)
        return 1;
    int mesh_json_count = (int)mesh_json_count64;
    {
        void *nodes_arr = jarr(root, "nodes");
        int64_t node_json_count64 = jarr_len(nodes_arr);
        if (node_json_count64 < 0 || node_json_count64 > INT32_MAX ||
            (size_t)node_json_count64 > SIZE_MAX / sizeof(rt_scene_node3d *))
            return 1;
        int node_json_count = (int)node_json_count64;
        if (node_json_count > 0) {
            int *node_parent = NULL;
            int graph_valid = gltf_validate_node_graph(nodes_arr, node_json_count, &node_parent);
            rt_scene_node3d **nodes =
                graph_valid ? (rt_scene_node3d **)calloc((size_t)node_json_count, sizeof(*nodes))
                            : NULL;
            if (!graph_valid || !nodes)
                load_failed = 1;
            for (int ni = 0; ni < node_json_count && nodes; ni++) {
                if (!gltf_build_template_node(asset,
                                              nodes_arr,
                                              ni,
                                              mesh_json_count,
                                              mesh_prim_start,
                                              mesh_prim_count,
                                              primitive_materials,
                                              primitive_variant_mappings,
                                              skins,
                                              skin_count,
                                              imported_lights,
                                              imported_light_count,
                                              &nodes[ni])) {
                    load_failed = 1;
                    continue;
                }
            }

            if (!load_failed &&
                !gltf_link_template_node_children(nodes_arr, node_json_count, nodes))
                load_failed = 1;
            if (!load_failed && graph_valid && nodes &&
                !gltf_import_node_scenes(
                    asset, root, nodes_arr, nodes, node_json_count, node_parent))
                load_failed = 1;

            for (int i = 0; i < node_json_count && nodes; i++)
                gltf_release_ref((void **)&nodes[i]);
            free(nodes);
            free(node_parent);
        }
    }
    return load_failed;
}
