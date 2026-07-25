//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/assets/rt_fbx_loader_morph.inc
// Purpose: Extract static and animated FBX blend-shape payloads into MorphTarget3D resources.
// Key invariants:
//   - Numeric connections resolve Geometry -> BlendShape -> channel -> Shape relationships.
//   - Shape deltas fan out through triangulation remaps before material-specific mesh cloning.
// Ownership/Lifetime:
//   - Published morph containers are retained by both the FBX asset and their source meshes.
//   - Per-channel shape indexes and thresholds are scratch metadata freed after animation import.
// Links: rt_fbx_loader.c, rt_fbx_loader_loader.inc, rt_fbx_loader_nodeanim.inc
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_fbx_loader_morph.inc
 * @brief Extracts FBX blend-shape channels, deltas, thresholds, and animation bindings.
 *
 * Shape Geometry payloads are converted into MorphTarget3D shapes attached to their
 * source meshes. Control-point deltas are expanded through polygon duplication remaps
 * or weighted procedural-surface contributions so each emitted runtime vertex receives
 * the correct position, normal, and tangent delta. Per-channel progressive thresholds
 * are retained as load-local metadata for the node-animation pass.
 */

/// @brief Read one finite floating-point FBX array element as a double.
/// @param property Float- or double-array property.
/// @param index Zero-based scalar index.
/// @param[out] out_value Receives the finite converted value.
/// @return Non-zero on success, or zero for an invalid property, type, index, or value.
static int fbx_morph_array_number(const fbx_prop_t *property, uint32_t index, double *out_value) {
    double value;
    if (!property || !out_value || (property->type != 'd' && property->type != 'f') ||
        index >= property->v.array.count || !property->v.array.data)
        return 0;
    if (property->type == 'd')
        value = ((const double *)property->v.array.data)[index];
    else if (property->type == 'f')
        value = (double)((const float *)property->v.array.data)[index];
    else
        return 0;
    if (!isfinite(value))
        return 0;
    *out_value = value;
    return 1;
}

/// @brief Read one FBX integer-array element as a range-checked 32-bit index.
/// @param property 32- or 64-bit signed integer-array property.
/// @param index Zero-based array index.
/// @param[out] out_index Receives the checked 32-bit value.
/// @return Non-zero on success, or zero for an invalid property, type, index, or range.
static int fbx_morph_array_index(const fbx_prop_t *property, uint32_t index, int32_t *out_index) {
    int64_t value;
    if (!property || !out_index || (property->type != 'i' && property->type != 'l') ||
        index >= property->v.array.count || !property->v.array.data)
        return 0;
    if (property->type == 'i')
        value = ((const int32_t *)property->v.array.data)[index];
    else if (property->type == 'l')
        value = ((const int64_t *)property->v.array.data)[index];
    else
        return 0;
    if (value < INT32_MIN || value > INT32_MAX)
        return 0;
    *out_index = (int32_t)value;
    return 1;
}

/// @brief Find the extracted mesh-array index associated with an FBX Geometry identifier.
/// @param bindings Geometry-ID to runtime-mesh binding array.
/// @param binding_count Number of initialized entries in @p bindings.
/// @param geometry_id FBX Geometry object identifier to match.
/// @return Zero-based binding/mesh index, or -1 when no match exists.
static int32_t fbx_morph_mesh_index(const fbx_mesh_binding_t *bindings,
                                    int32_t binding_count,
                                    int64_t geometry_id) {
    if (!bindings || binding_count <= 0)
        return -1;
    for (int32_t i = 0; i < binding_count; ++i)
        if (bindings[i].id == geometry_id)
            return i;
    return -1;
}

/// @brief Find a connected parent object with a required node name and optional subtype.
/// @param objects Parsed FBX `Objects` node used for identifier lookup.
/// @param ct Connection table containing child-to-parent adjacency.
/// @param child_id Identifier whose outgoing parent connections are searched.
/// @param node_name Required parent node name.
/// @param subtype Optional required third-property subtype, or NULL to accept any subtype.
/// @return Borrowed pointer to the first matching parent object, or NULL when none exists.
static fbx_node_t *fbx_morph_find_parent(fbx_node_t *objects,
                                         const fbx_conn_table_t *ct,
                                         int64_t child_id,
                                         const char *node_name,
                                         const char *subtype) {
    for (int32_t connection = fbx_connections_first_for_child(ct, child_id); connection >= 0;
         connection = fbx_connections_next_for_child(ct, connection)) {
        fbx_node_t *parent = fbx_find_object_by_id(objects, ct->entries[connection].parent_id);
        if (parent && strcmp(parent->name, node_name) == 0 && parent->prop_count >= 3 &&
            (!subtype || strcmp(fbx_prop_str(parent, 2), subtype) == 0)) {
            return parent;
        }
    }
    return NULL;
}

/// @brief Release native metadata used to translate BlendShapeChannel animation curves.
/// @param[in,out] bindings Heap binding array and its owned shape/threshold arrays.
/// @param count Number of initialized entries in @p bindings.
static void fbx_morph_channel_bindings_free(fbx_morph_channel_binding_t *bindings, int32_t count) {
    if (!bindings)
        return;
    for (int32_t i = 0; i < count; ++i) {
        free(bindings[i].shape_indices);
        free(bindings[i].full_weights);
    }
    free(bindings);
}

/// @brief Apply one imported control-point delta to all corresponding runtime vertices.
/// @details Procedural meshes distribute and accumulate the delta using tensor-product
///          control weights. Polygon meshes fan it out through the control-to-emitted
///          vertex lists. A direct vertex-index fallback is used when no remap is available.
/// @param[in,out] morph Runtime MorphTarget3D resource to update.
/// @param shape Runtime shape index receiving the delta.
/// @param remap Optional source-control remap for the mesh.
/// @param mesh Source runtime mesh used to validate vertex bounds.
/// @param control_index Source FBX control-point index.
/// @param delta_kind Delta selector: zero for position, one for normal, two for tangent.
/// @param x X component of the imported delta.
/// @param y Y component of the imported delta.
/// @param z Z component of the imported delta.
static void fbx_morph_set_remapped_delta(void *morph,
                                         int64_t shape,
                                         const fbx_mesh_remap_t *remap,
                                         const rt_mesh3d *mesh,
                                         int32_t control_index,
                                         int delta_kind,
                                         double x,
                                         double y,
                                         double z) {
    if (!morph || !mesh || control_index < 0)
        return;
    if (remap && remap->surface_vertices && remap->surface_control_indices &&
        remap->surface_control_weights && control_index < remap->control_count &&
        remap->surface_vertex_count == mesh->vertex_count) {
        for (uint32_t vertex = 0; vertex < remap->surface_vertex_count; ++vertex) {
            const fbx_surface_vertex_remap_t *range = &remap->surface_vertices[vertex];
            double coefficient = 0.0;
            rt_morphtarget3d_shape_view_internal view;
            const float *current = NULL;
            if (range->offset > remap->surface_contribution_count ||
                range->count > remap->surface_contribution_count - range->offset)
                continue;
            for (uint32_t contribution = 0; contribution < range->count; ++contribution) {
                uint32_t index = range->offset + contribution;
                if (remap->surface_control_indices[index] == control_index)
                    coefficient += remap->surface_control_weights[index];
            }
            if (!isfinite(coefficient) || fabs(coefficient) <= 1e-15 ||
                !rt_morphtarget3d_get_shape_view_internal(morph, shape, &view))
                continue;
            if (delta_kind == 1)
                current = view.normal_deltas;
            else if (delta_kind == 2)
                current = view.tangent_deltas;
            else
                current = view.position_deltas;
            double old_x = current ? current[(size_t)vertex * 3u + 0u] : 0.0;
            double old_y = current ? current[(size_t)vertex * 3u + 1u] : 0.0;
            double old_z = current ? current[(size_t)vertex * 3u + 2u] : 0.0;
            if (delta_kind == 1)
                rt_morphtarget3d_set_normal_delta(morph,
                                                  shape,
                                                  vertex,
                                                  old_x + coefficient * x,
                                                  old_y + coefficient * y,
                                                  old_z + coefficient * z);
            else if (delta_kind == 2)
                rt_morphtarget3d_set_tangent_delta(morph,
                                                   shape,
                                                   vertex,
                                                   old_x + coefficient * x,
                                                   old_y + coefficient * y,
                                                   old_z + coefficient * z);
            else
                rt_morphtarget3d_set_delta(morph,
                                           shape,
                                           vertex,
                                           old_x + coefficient * x,
                                           old_y + coefficient * y,
                                           old_z + coefficient * z);
        }
    } else if (remap && remap->control_vertices && control_index < remap->control_count) {
        const fbx_vertex_index_list_t *vertices = &remap->control_vertices[control_index];
        for (int32_t i = 0; i < vertices->count; ++i) {
            int64_t vertex = (int64_t)vertices->vertices[i];
            if (delta_kind == 1)
                rt_morphtarget3d_set_normal_delta(morph, shape, vertex, x, y, z);
            else if (delta_kind == 2)
                rt_morphtarget3d_set_tangent_delta(morph, shape, vertex, x, y, z);
            else
                rt_morphtarget3d_set_delta(morph, shape, vertex, x, y, z);
        }
    } else if ((uint32_t)control_index < mesh->vertex_count) {
        if (delta_kind == 1)
            rt_morphtarget3d_set_normal_delta(morph, shape, control_index, x, y, z);
        else if (delta_kind == 2)
            rt_morphtarget3d_set_tangent_delta(morph, shape, control_index, x, y, z);
        else
            rt_morphtarget3d_set_delta(morph, shape, control_index, x, y, z);
    }
}

/// @brief Import BlendShapeChannel metadata and deltas, then attach morphs to source meshes.
/// @details Resolves each `Geometry -> BlendShape -> BlendShapeChannel -> Shape`
///          connection chain, creates one morph container per affected mesh, imports
///          progressive shape thresholds, and records channel bindings for animation.
///          Invalid optional normal/tangent payloads and thresholds produce warnings;
///          hard allocation or budget failures roll back the entire pass.
/// @param[in,out] asset Staged FBX asset that receives owned morph-target references.
/// @param objects Parsed FBX `Objects` node.
/// @param ct Validated object connection table.
/// @param z_up Non-zero to convert imported deltas from Z-up to runtime Y-up coordinates.
/// @param mesh_bindings Geometry-ID to mesh binding array.
/// @param mesh_binding_count Number of entries in @p mesh_bindings.
/// @param mesh_remaps Geometry control-to-runtime-vertex remap array.
/// @param mesh_remap_count Number of entries in @p mesh_remaps.
/// @param[out] out_bindings Receives the newly allocated channel-binding array.
/// @param[out] out_binding_count Receives the number of initialized channel bindings.
/// @param[in,out] context Loader context charged for morph and binding allocations.
/// @return Nonzero on success; zero on allocation/budget failure so the whole load rolls back.
static int fbx_load_extract_morphs(rt_fbx_asset *asset,
                                   fbx_node_t *objects,
                                   const fbx_conn_table_t *ct,
                                   int z_up,
                                   const fbx_mesh_binding_t *mesh_bindings,
                                   int32_t mesh_binding_count,
                                   fbx_mesh_remap_t *mesh_remaps,
                                   int32_t mesh_remap_count,
                                   fbx_morph_channel_binding_t **out_bindings,
                                   int32_t *out_binding_count,
                                   fbx_load_context_t *context) {
    fbx_morph_channel_binding_t *bindings = NULL;
    int32_t binding_count = 0;
    int32_t mesh_count;
    if (!asset || !objects || !ct || !out_bindings || !out_binding_count || !context)
        return 0;
    *out_bindings = NULL;
    *out_binding_count = 0;
    mesh_count = fbx_asset_safe_count(asset->meshes, asset->mesh_count, asset->mesh_capacity);
    if (mesh_count <= 0)
        return 1;
    if ((size_t)mesh_count > SIZE_MAX / sizeof(void *) ||
        !fbx_budget_charge(context, (uint64_t)mesh_count, sizeof(void *)))
        return 0;
    asset->morph_targets = (void **)calloc((size_t)mesh_count, sizeof(void *));
    if (!asset->morph_targets)
        return 0;
    asset->morph_count = mesh_count;
    asset->morph_capacity = mesh_count;

    for (int32_t object_index = 0; object_index < objects->child_count; ++object_index) {
        fbx_node_t *channel = &objects->children[object_index];
        fbx_node_t *blend_shape;
        fbx_node_t *geometry;
        fbx_node_t *full_weights_node;
        const fbx_prop_t *full_weights_property = NULL;
        int32_t shape_candidate_count = 0;
        int32_t mesh_index;
        int32_t valid_shape_count = 0;
        int warned_weights = 0;
        int created_morph = 0;
        int32_t *shape_indices = NULL;
        double *full_weights = NULL;
        void *morph;
        rt_mesh3d *mesh;
        const fbx_mesh_remap_t *remap;
        if (strcmp(channel->name, "Deformer") != 0 || channel->prop_count < 3 ||
            strcmp(fbx_prop_str(channel, 2), "BlendShapeChannel") != 0)
            continue;
        blend_shape =
            fbx_morph_find_parent(objects, ct, fbx_prop_i64(channel, 0), "Deformer", "BlendShape");
        geometry =
            blend_shape
                ? fbx_morph_find_parent(objects, ct, fbx_prop_i64(blend_shape, 0), "Geometry", NULL)
                : NULL;
        if (!geometry || !fbx_geometry_type_is_renderable(fbx_prop_str(geometry, 2)))
            continue;
        mesh_index =
            fbx_morph_mesh_index(mesh_bindings, mesh_binding_count, fbx_prop_i64(geometry, 0));
        if (mesh_index < 0 || mesh_index >= mesh_count)
            continue;
        for (int32_t connection = fbx_connections_first_for_parent(ct, fbx_prop_i64(channel, 0));
             connection >= 0;
             connection = fbx_connections_next_for_parent(ct, connection)) {
            fbx_node_t *shape = fbx_find_object_by_id(objects, ct->entries[connection].child_id);
            if (shape && strcmp(shape->name, "Geometry") == 0 && shape->prop_count >= 3 &&
                strcmp(fbx_prop_str(shape, 2), "Shape") == 0) {
                ++shape_candidate_count;
            }
        }
        if (shape_candidate_count <= 0)
            continue;
        if ((size_t)shape_candidate_count > SIZE_MAX / sizeof(*shape_indices) ||
            (size_t)shape_candidate_count > SIZE_MAX / sizeof(*full_weights) ||
            !fbx_budget_charge(context,
                               (uint64_t)shape_candidate_count,
                               sizeof(*shape_indices) + sizeof(*full_weights))) {
            goto fail;
        }
        shape_indices = (int32_t *)malloc((size_t)shape_candidate_count * sizeof(*shape_indices));
        full_weights = (double *)malloc((size_t)shape_candidate_count * sizeof(*full_weights));
        if (!shape_indices || !full_weights) {
            free(shape_indices);
            free(full_weights);
            goto fail;
        }
        mesh = (rt_mesh3d *)asset->meshes[mesh_index];
        if (!mesh || mesh->vertex_count == 0 || mesh->vertex_count > (uint32_t)INT32_MAX) {
            free(shape_indices);
            free(full_weights);
            continue;
        }
        remap = fbx_find_mesh_remap(mesh_remaps, mesh_remap_count, fbx_prop_i64(geometry, 0));
        morph = asset->morph_targets[mesh_index];
        if (!morph) {
            if (!fbx_budget_charge(context, 1u, 256u)) {
                free(shape_indices);
                free(full_weights);
                goto fail;
            }
            morph = rt_morphtarget3d_new((int64_t)mesh->vertex_count);
            if (!morph) {
                free(shape_indices);
                free(full_weights);
                goto fail;
            }
            asset->morph_targets[mesh_index] = morph;
            rt_mesh3d_set_morph_targets(mesh, morph);
            created_morph = 1;
        }
        full_weights_node = fbx_find_child(channel, "FullWeights");
        if (full_weights_node && full_weights_node->prop_count > 0)
            full_weights_property = &full_weights_node->props[0];

        int32_t candidate_index = 0;
        for (int32_t connection = fbx_connections_first_for_parent(ct, fbx_prop_i64(channel, 0));
             connection >= 0;
             connection = fbx_connections_next_for_parent(ct, connection)) {
            fbx_node_t *shape = fbx_find_object_by_id(objects, ct->entries[connection].child_id);
            fbx_node_t *index_node;
            fbx_node_t *position_node;
            fbx_node_t *normal_node;
            fbx_node_t *tangent_node;
            const fbx_prop_t *index_property;
            const fbx_prop_t *position_property;
            const fbx_prop_t *normal_property = NULL;
            const fbx_prop_t *tangent_property = NULL;
            uint32_t delta_count;
            int64_t shape_index;
            double threshold = 0.0;
            if (!shape || strcmp(shape->name, "Geometry") != 0 || shape->prop_count < 3 ||
                strcmp(fbx_prop_str(shape, 2), "Shape") != 0)
                continue;
            index_node = fbx_find_child(shape, "Indexes");
            position_node = fbx_find_child(shape, "Vertices");
            normal_node = fbx_find_child(shape, "Normals");
            tangent_node = fbx_find_child(shape, "Tangents");
            if (!index_node || !position_node || index_node->prop_count < 1 ||
                position_node->prop_count < 1) {
                rt_asset_error_add_warningf("FBX blend shape '%s' has no index/vertex payload",
                                            fbx_prop_str(shape, 1));
                ++candidate_index;
                continue;
            }
            index_property = &index_node->props[0];
            position_property = &position_node->props[0];
            if ((index_property->type != 'i' && index_property->type != 'l') ||
                (position_property->type != 'd' && position_property->type != 'f') ||
                !index_property->v.array.data || !position_property->v.array.data) {
                rt_asset_error_add_warningf("FBX blend shape '%s' has an invalid delta payload",
                                            fbx_prop_str(shape, 1));
                ++candidate_index;
                continue;
            }
            delta_count = index_property->v.array.count;
            if (delta_count == 0 || delta_count > (uint32_t)INT32_MAX ||
                position_property->v.array.count / 3u < delta_count) {
                rt_asset_error_add_warningf("FBX blend shape '%s' has an invalid delta payload",
                                            fbx_prop_str(shape, 1));
                ++candidate_index;
                continue;
            }
            if (normal_node && normal_node->prop_count > 0) {
                const fbx_prop_t *candidate = &normal_node->props[0];
                if ((candidate->type == 'd' || candidate->type == 'f') &&
                    candidate->v.array.count / 3u >= delta_count)
                    normal_property = candidate;
                else
                    rt_asset_error_add_warningf(
                        "FBX blend shape '%s' has invalid normal deltas; normals were omitted",
                        fbx_prop_str(shape, 1));
            }
            if (tangent_node && tangent_node->prop_count > 0) {
                const fbx_prop_t *candidate = &tangent_node->props[0];
                if ((candidate->type == 'd' || candidate->type == 'f') &&
                    candidate->v.array.count / 3u >= delta_count)
                    tangent_property = candidate;
                else
                    rt_asset_error_add_warningf(
                        "FBX blend shape '%s' has invalid tangent deltas; tangents were omitted",
                        fbx_prop_str(shape, 1));
            }
            if (!fbx_budget_charge(
                    context,
                    (uint64_t)mesh->vertex_count,
                    (uint64_t)(1 + (normal_property ? 1 : 0) + (tangent_property ? 1 : 0)) * 3u *
                        sizeof(float))) {
                free(shape_indices);
                free(full_weights);
                goto fail;
            }
            rt_string shape_name = fbx_object_name_string(shape, 1);
            if (!shape_name || rt_str_is_empty(shape_name)) {
                rt_string_unref(shape_name);
                shape_name = rt_const_cstr("Shape");
            }
            shape_index = rt_morphtarget3d_add_shape(morph, shape_name);
            rt_string_unref(shape_name);
            if (shape_index < 0) {
                free(shape_indices);
                free(full_weights);
                goto fail;
            }
            for (uint32_t delta = 0; delta < delta_count; ++delta) {
                int32_t control_index;
                double values[3];
                if (!fbx_morph_array_index(index_property, delta, &control_index) ||
                    !fbx_morph_array_number(position_property, delta * 3u, &values[0]) ||
                    !fbx_morph_array_number(position_property, delta * 3u + 1u, &values[1]) ||
                    !fbx_morph_array_number(position_property, delta * 3u + 2u, &values[2]))
                    continue;
                if (z_up) {
                    double old_y = values[1];
                    values[1] = values[2];
                    values[2] = -old_y;
                }
                if (fbx_sanitize_position3(&values[0], &values[1], &values[2]))
                    fbx_morph_set_remapped_delta(morph,
                                                 shape_index,
                                                 remap,
                                                 mesh,
                                                 control_index,
                                                 0,
                                                 values[0],
                                                 values[1],
                                                 values[2]);
                const fbx_prop_t *optional_properties[2] = {normal_property, tangent_property};
                for (int kind = 0; kind < 2; ++kind) {
                    const fbx_prop_t *property = optional_properties[kind];
                    if (!property || !fbx_morph_array_number(property, delta * 3u, &values[0]) ||
                        !fbx_morph_array_number(property, delta * 3u + 1u, &values[1]) ||
                        !fbx_morph_array_number(property, delta * 3u + 2u, &values[2]))
                        continue;
                    if (z_up) {
                        double old_y = values[1];
                        values[1] = values[2];
                        values[2] = -old_y;
                    }
                    if (fbx_sanitize_position3(&values[0], &values[1], &values[2]))
                        fbx_morph_set_remapped_delta(morph,
                                                     shape_index,
                                                     remap,
                                                     mesh,
                                                     control_index,
                                                     kind + 1,
                                                     values[0],
                                                     values[1],
                                                     values[2]);
                }
            }
            if (!full_weights_property ||
                !fbx_morph_array_number(
                    full_weights_property, (uint32_t)candidate_index, &threshold) ||
                threshold <= 0.0 ||
                (valid_shape_count > 0 && threshold <= full_weights[valid_shape_count - 1])) {
                threshold = 100.0 * (double)(valid_shape_count + 1);
                if (!warned_weights) {
                    rt_asset_error_add_warningf("FBX BlendShapeChannel '%s' has invalid "
                                                "FullWeights; using progressive 100%% steps",
                                                fbx_prop_str(channel, 1));
                    warned_weights = 1;
                }
            }
            shape_indices[valid_shape_count] = (int32_t)shape_index;
            full_weights[valid_shape_count] = threshold;
            ++valid_shape_count;
            ++candidate_index;
        }
        if (valid_shape_count == 0) {
            if (created_morph) {
                rt_mesh3d_set_morph_targets(mesh, NULL);
                fbx_release_ref(&asset->morph_targets[mesh_index]);
            }
            free(shape_indices);
            free(full_weights);
            continue;
        }
        if (binding_count == INT32_MAX ||
            (size_t)(binding_count + 1) > SIZE_MAX / sizeof(*bindings) ||
            !fbx_budget_charge(context, 1u, sizeof(*bindings))) {
            free(shape_indices);
            free(full_weights);
            goto fail;
        }
        fbx_morph_channel_binding_t *grown = (fbx_morph_channel_binding_t *)realloc(
            bindings, (size_t)(binding_count + 1) * sizeof(*bindings));
        if (!grown) {
            free(shape_indices);
            free(full_weights);
            goto fail;
        }
        bindings = grown;
        memset(&bindings[binding_count], 0, sizeof(bindings[binding_count]));
        bindings[binding_count].channel_id = fbx_prop_i64(channel, 0);
        bindings[binding_count].geometry_id = fbx_prop_i64(geometry, 0);
        bindings[binding_count].mesh_index = mesh_index;
        bindings[binding_count].shape_count = valid_shape_count;
        bindings[binding_count].shape_indices = shape_indices;
        bindings[binding_count].full_weights = full_weights;
        bindings[binding_count].default_percent =
            fbx_read_object_number(channel, "DeformPercent", 0.0, NULL);
        for (int32_t local_shape = 0; local_shape < valid_shape_count; ++local_shape) {
            rt_morphtarget3d_set_weight(
                morph,
                shape_indices[local_shape],
                fbx_morph_progressive_shape_weight(&bindings[binding_count],
                                                   bindings[binding_count].default_percent,
                                                   local_shape));
        }
        ++binding_count;
    }
    *out_bindings = bindings;
    *out_binding_count = binding_count;
    return 1;
fail:
    fbx_morph_channel_bindings_free(bindings, binding_count);
    return 0;
}
