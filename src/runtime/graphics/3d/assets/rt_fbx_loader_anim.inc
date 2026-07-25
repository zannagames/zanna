//===----------------------------------------------------------------------===//
// Part of the Zanna project, under the GNU GPL v3. See LICENSE.
// File: src/runtime/graphics/3d/assets/rt_fbx_loader_anim.inc  (#included from rt_fbx_loader.c; not
// a standalone translation unit) Purpose: FBX animation extraction
//===----------------------------------------------------------------------===//
/**
 * @file rt_fbx_loader_anim.inc
 * @brief Imports FBX skin weights and skeletal animation into runtime objects.
 *
 * This fragment reduces arbitrary cluster influences to the runtime's
 * four-weight vertex layout, validates and samples constant, linear, and cubic
 * animation curves, composes FBX animation layers and inheritance modes, bakes
 * constraint-dependent poses, and transactionally emits `Animation3D` clips.
 * It is included by `rt_fbx_loader.c` after the parser, geometry, scene, and
 * constraint helpers it consumes.
 */
/*==========================================================================
 * Animation extraction
 *=========================================================================*/

/// Number of FBX time ticks in one second.
#define FBX_TIME_SECOND 46186158000LL

/// @brief Locate a child node named @p child_name and return its first property as an int64 array.
/// @details Expects the property type byte to be `'l'` (FBX long-array).  Writes the element
///          count to *count and returns a pointer into the already-decoded in-memory array.
///          Returns NULL and leaves *count unchanged on a missing child or wrong property type.
/// @param[in] node Parent node to search.
/// @param[in] child_name Exact child-node name.
/// @param[out] count Required destination for the decoded element count on success.
/// @return Borrowed decoded `int64_t` array data, or `NULL`.
static const int64_t *fbx_get_i64_array(fbx_node_t *node, const char *child_name, uint32_t *count) {
    fbx_node_t *child = fbx_find_child(node, child_name);
    if (!child || child->prop_count < 1)
        return NULL;
    fbx_prop_t *p = &child->props[0];
    if (p->type == 'l' && p->v.array.data) {
        *count = p->v.array.count;
        return (const int64_t *)p->v.array.data;
    }
    return NULL;
}

/// @brief Locate a child node named @p child_name and return its first property as an int32 array.
/// @details Expects property type `'i'` (FBX int-array).  Used to read per-vertex indices such
///          as cluster Indexes.  Returns NULL and leaves *count unchanged on type mismatch.
/// @param[in] node Parent node to search.
/// @param[in] child_name Exact child-node name.
/// @param[out] count Required destination for the decoded element count on success.
/// @return Borrowed decoded `int32_t` array data, or `NULL`.
static const int32_t *fbx_get_i32_array(fbx_node_t *node, const char *child_name, uint32_t *count) {
    fbx_node_t *child = fbx_find_child(node, child_name);
    if (!child || child->prop_count < 1)
        return NULL;
    fbx_prop_t *p = &child->props[0];
    if (p->type == 'i' && p->v.array.data) {
        *count = p->v.array.count;
        return (const int32_t *)p->v.array.data;
    }
    return NULL;
}

/// @brief Locate a child node named @p child_name and return its first property as a double array.
/// @details Expects property type `'d'` (FBX double-array). When the property is type `'f'`
///          (float-array), writes *count but returns NULL — callers should fall back to
///          fbx_get_f32_array in that case. Used for animation curve values and blend weights.
/// @param[in] node Parent node to search.
/// @param[in] child_name Exact child-node name.
/// @param[out] count Required destination for the decoded element count on a
/// double or float array.
/// @return Borrowed decoded `double` array data, or `NULL`.
static const double *fbx_get_f64_array(fbx_node_t *node, const char *child_name, uint32_t *count) {
    fbx_node_t *child = fbx_find_child(node, child_name);
    if (!child || child->prop_count < 1)
        return NULL;
    fbx_prop_t *p = &child->props[0];
    if (p->type == 'd' && p->v.array.data) {
        *count = p->v.array.count;
        return (const double *)p->v.array.data;
    }
    /* Also handle float arrays by reading as float */
    if (p->type == 'f' && p->v.array.data) {
        *count = p->v.array.count;
        return NULL; /* caller must handle float arrays separately */
    }
    return NULL;
}

/// @brief Convenience: locate `node->child_name` and return its float-array data + count.
/// Returns NULL on missing child or non-float-array property.
/// @param[in] node Parent node to search.
/// @param[in] child_name Exact child-node name.
/// @param[out] count Required destination for the decoded element count on success.
/// @return Borrowed decoded `float` array data, or `NULL`.
static const float *fbx_get_f32_array(fbx_node_t *node, const char *child_name, uint32_t *count) {
    fbx_node_t *child = fbx_find_child(node, child_name);
    if (!child || child->prop_count < 1)
        return NULL;
    fbx_prop_t *p = &child->props[0];
    if (p->type == 'f' && p->v.array.data) {
        *count = p->v.array.count;
        return (const float *)p->v.array.data;
    }
    return NULL;
}

/// @brief Resolve an FBX Model numeric ID to its engine bone index.
/// @details Object IDs are the sole identity key. Display-name fallback is intentionally forbidden
///          because duplicate or long equal-prefix names are legal FBX content and must never bind
///          animation/skin data to a different bone. The object/skeleton parameters remain in the
///          internal signature for source compatibility with extraction call sites.
/// @param[in] objects Unused objects tree retained for source-compatible call sites.
/// @param[in] skeleton Unused target skeleton retained for source-compatible call sites.
/// @param[in] bone_bindings Numeric model-to-bone mapping table.
/// @param[in] bone_binding_count Number of mapping entries.
/// @param[in] model_id FBX model identifier to resolve.
/// @return Engine bone index from the load-local binding table, or -1 when the ID is not a bone.
static int32_t fbx_find_bone_index_for_model(fbx_node_t *objects,
                                             void *skeleton,
                                             const fbx_bone_binding_t *bone_bindings,
                                             int32_t bone_binding_count,
                                             int64_t model_id) {
    (void)objects;
    (void)skeleton;
    if (!bone_bindings || bone_binding_count <= 0 || model_id == 0)
        return -1;
    for (int32_t i = 0; i < bone_binding_count; i++) {
        if (bone_bindings[i].model_id == model_id && bone_bindings[i].bone_index >= 0)
            return bone_bindings[i].bone_index;
    }
    return -1;
}

/// @brief Find the Model node that owns the given Cluster deformer in the FBX connection table.
/// @details A Cluster (sub-deformer) is connected child→parent to the Model node representing
///          the bone it drives. This function walks the connection table for an entry whose
///          child_id is @p cluster_id and whose parent is a "Model" node.
/// @param[in] objects FBX Objects node used to validate parent object types.
/// @param[in] ct Indexed connection table.
/// @param[in] cluster_id Cluster deformer identifier.
/// @return FBX object ID of the owning Model, or 0 if not found.
static int64_t fbx_find_cluster_bone_model(fbx_node_t *objects,
                                           const fbx_conn_table_t *ct,
                                           int64_t cluster_id) {
    if (!objects || !ct)
        return 0;
    for (int32_t i = fbx_connections_first_for_child(ct, cluster_id); i >= 0;
         i = fbx_connections_next_for_child(ct, i)) {
        fbx_node_t *parent;
        parent = fbx_find_object_by_id(objects, ct->entries[i].parent_id);
        if (parent && strcmp(parent->name, "Model") == 0)
            return ct->entries[i].parent_id;
    }
    return 0;
}

/// @brief Walk the FBX connection table to find the geometry that owns a given skin.
/// @details In FBX, a Deformer (skin) is connected OO (object→object) upward to a
///          renderable Geometry node. This scan finds the first supported geometry
///          parent, which is the mesh or tessellated surface the skin drives.
/// @param[in] objects FBX Objects node used to validate parent object types.
/// @param[in] ct Indexed connection table.
/// @param[in] skin_id Skin deformer identifier.
/// @return Geometry object ID, or 0 if not found / not a mesh skin.
static int64_t fbx_find_skin_geometry(fbx_node_t *objects,
                                      const fbx_conn_table_t *ct,
                                      int64_t skin_id) {
    if (!objects || !ct)
        return 0;
    for (int32_t i = fbx_connections_first_for_child(ct, skin_id); i >= 0;
         i = fbx_connections_next_for_child(ct, i)) {
        fbx_node_t *parent;
        parent = fbx_find_object_by_id(objects, ct->entries[i].parent_id);
        if (parent && strcmp(parent->name, "Geometry") == 0 && parent->prop_count >= 3 &&
            fbx_geometry_type_is_renderable(fbx_prop_str(parent, 2)))
            return ct->entries[i].parent_id;
    }
    return 0;
}

/// @brief Insert a (bone, weight) influence into a 4-slot fixed-size influence record,
///        keeping the four largest weights.
/// @details GPU skinning palettes are capped at 4 influences per vertex. FBX clusters
///          can contribute an arbitrary number of (bone, weight) pairs per vertex, so
///          the loader must reduce them to the top four. Strategy:
///            1. If `bone_index` is already present, fold the weight in (handles
///               duplicate cluster entries that target the same bone/vertex pair).
///            2. Else, if any slot is empty (weight <= 0), fill it.
///            3. Else replace the weakest slot only when the new weight is strictly
///               greater — this drops the smallest contribution and keeps the four
///               most significant.
///          Weights are not renormalized here; that happens in
///          `fbx_apply_control_skin_to_vertex` after all clusters are accumulated.
/// @param[in,out] dst Destination influence (mutated in place).
/// @param[in] bone_index Engine bone index to add.
/// @param[in] weight Non-negative weight; values <= 0 are silently dropped.
static void fbx_skin_add_influence(fbx_skin_influence_t *dst, int32_t bone_index, double weight) {
    int32_t weakest = 0;
    if (!dst || bone_index < 0 || !isfinite(weight) || weight <= 0.0)
        return;
    if (weight > FBX_SKIN_WEIGHT_MAX)
        weight = FBX_SKIN_WEIGHT_MAX;
    for (int i = 0; i < 4; i++) {
        if (dst->weights[i] > 0.0 && dst->bone_indices[i] == bone_index) {
            dst->weights[i] += weight;
            if (!isfinite(dst->weights[i]) || dst->weights[i] > FBX_SKIN_WEIGHT_MAX)
                dst->weights[i] = FBX_SKIN_WEIGHT_MAX;
            return;
        }
    }
    for (int i = 0; i < 4; i++) {
        if (dst->weights[i] <= 0.0) {
            dst->bone_indices[i] = bone_index;
            dst->weights[i] = weight;
            return;
        }
        if (dst->weights[i] < dst->weights[weakest])
            weakest = i;
    }
    if (weight > dst->weights[weakest]) {
        dst->bone_indices[weakest] = bone_index;
        dst->weights[weakest] = weight;
    }
}

/// @brief Finalize and write a per-vertex influence record to the mesh's skinning data.
/// @details Copies up to 4 (bone, weight) pairs from `influence`, filters out empty
///          slots (weight <= 0), renormalizes the surviving weights to sum to 1, and
///          commits them via `rt_mesh3d_set_bone_weights`. Updates `mesh->bone_count`
///          as a running high-water mark so the skinning palette is sized correctly
///          for the shader upload. If every influence slot is empty, all four stored
///          indices and weights on the vertex are cleared.
/// @param[in,out] mesh Mesh receiving normalized weights.
/// @param[in] vertex_index Zero-based output vertex index.
/// @param[in] influence Accumulated four-slot control-point influence.
static void fbx_apply_control_skin_to_vertex(rt_mesh3d *mesh,
                                             int32_t vertex_index,
                                             const fbx_skin_influence_t *influence) {
    double weights[4];
    double sum = 0.0;
    int64_t bones[4];
    if (!mesh || !influence || vertex_index < 0 || vertex_index >= (int32_t)mesh->vertex_count)
        return;
    for (int i = 0; i < 4; i++) {
        bones[i] = influence->weights[i] > 0.0 && influence->bone_indices[i] >= 0
                       ? influence->bone_indices[i]
                       : 0;
        weights[i] = isfinite(influence->weights[i]) && influence->weights[i] > 0.0 &&
                             influence->bone_indices[i] >= 0
                         ? influence->weights[i]
                         : 0.0;
        if (weights[i] > FBX_SKIN_WEIGHT_MAX)
            weights[i] = FBX_SKIN_WEIGHT_MAX;
        sum += weights[i];
    }
    if (!isfinite(sum) || sum <= 0.0) {
        memset(mesh->vertices[vertex_index].bone_indices,
               0,
               sizeof(mesh->vertices[vertex_index].bone_indices));
        memset(mesh->vertices[vertex_index].bone_weights,
               0,
               sizeof(mesh->vertices[vertex_index].bone_weights));
        return;
    }
    for (int i = 0; i < 4; i++) {
        weights[i] /= sum;
        if (weights[i] > 0.0 && bones[i] + 1 > mesh->bone_count)
            mesh->bone_count = (int32_t)(bones[i] + 1);
    }
    rt_mesh3d_set_bone_weights(mesh,
                               vertex_index,
                               bones[0],
                               weights[0],
                               bones[1],
                               weights[1],
                               bones[2],
                               weights[2],
                               bones[3],
                               weights[3]);
}

/// @brief Stamp a fully-accumulated control-point influence table onto every vertex
///        of the mesh, respecting the control-point → vertex fan-out recorded in
///        `remap`.
/// @details FBX stores skinning at the "control point" granularity (the canonical
///          unique vertex positions), not the triangulated / duplicated vertices the
///          GPU consumes. When a control point maps to multiple mesh vertices (common
///          for hard edges where positions share but normals diverge), each of those
///          duplicate vertices must receive the same influences. The remap's
///          `control_vertices[ci]` is a flat list of mesh vertex indices spawned by
///          control point `ci` — we expand that fan-out here. If no remap exists we
///          assume 1:1 (no deduplication happened during load).
/// @param[in,out] mesh_obj Runtime mesh receiving weights.
/// @param[in] remap Optional control/surface-to-output mapping.
/// @param[in] controls Per-control-point accumulated influences.
/// @param[in] control_count Number of control influence records.
static void fbx_apply_skin_to_mesh(void *mesh_obj,
                                   const fbx_mesh_remap_t *remap,
                                   const fbx_skin_influence_t *controls,
                                   int32_t control_count) {
    rt_mesh3d *mesh = (rt_mesh3d *)mesh_obj;
    if (!mesh || !controls || control_count <= 0)
        return;
    if (remap && remap->surface_vertices && remap->surface_control_indices &&
        remap->surface_control_weights && remap->surface_vertex_count == mesh->vertex_count) {
        for (uint32_t vertex = 0; vertex < remap->surface_vertex_count; ++vertex) {
            const fbx_surface_vertex_remap_t *range = &remap->surface_vertices[vertex];
            fbx_skin_influence_t blended;
            memset(&blended, 0, sizeof(blended));
            if (range->offset > remap->surface_contribution_count ||
                range->count > remap->surface_contribution_count - range->offset)
                continue;
            for (uint32_t contribution = 0; contribution < range->count; ++contribution) {
                uint32_t index = range->offset + contribution;
                int32_t control = remap->surface_control_indices[index];
                double basis_weight = remap->surface_control_weights[index];
                if (control < 0 || control >= control_count || !isfinite(basis_weight) ||
                    basis_weight <= 0.0)
                    continue;
                for (int influence = 0; influence < 4; ++influence) {
                    if (controls[control].bone_indices[influence] >= 0 &&
                        controls[control].weights[influence] > 0.0) {
                        fbx_skin_add_influence(&blended,
                                               controls[control].bone_indices[influence],
                                               basis_weight * controls[control].weights[influence]);
                    }
                }
            }
            fbx_apply_control_skin_to_vertex(mesh, (int32_t)vertex, &blended);
        }
        return;
    }
    for (int32_t ci = 0; ci < control_count; ci++) {
        if (remap && remap->control_vertices && ci < remap->control_count) {
            const fbx_vertex_index_list_t *list = &remap->control_vertices[ci];
            for (int32_t vi = 0; vi < list->count; vi++)
                fbx_apply_control_skin_to_vertex(mesh, list->vertices[vi], &controls[ci]);
        } else {
            fbx_apply_control_skin_to_vertex(mesh, ci, &controls[ci]);
        }
    }
}

/// @brief Apply all FBX Skin deformers to their target meshes, populating bone weights.
/// @details Iterates every Skin Deformer in the Objects section. For each skin it resolves
///          the target geometry (via `fbx_find_skin_geometry`), finds the engine mesh object
///          (via @p mesh_bindings), allocates a per-control-point influence accumulator, then
///          iterates all connected Cluster sub-deformers to accumulate (bone, weight) pairs.
///          Finalization (renormalization, write to mesh) is done by `fbx_apply_skin_to_mesh`.
///          Float and double weight arrays are both handled; the remap table is used when the
///          mesh was deduplicated to map control-point indices back to the original FBX layout.
/// @param[in] objects FBX Objects node.
/// @param[in] ct Indexed connection graph.
/// @param[in] skeleton Runtime skeleton receiving mesh bindings.
/// @param[in] bone_bindings Numeric model-to-bone mapping table.
/// @param[in] bone_binding_count Number of bone mappings.
/// @param[in] mesh_bindings Geometry-to-runtime-mesh mapping table.
/// @param[in] mesh_binding_count Number of mesh mappings.
/// @param[in] mesh_remaps Optional geometry remapping records.
/// @param[in] mesh_remap_count Number of remapping records.
static void fbx_apply_skinning(fbx_node_t *objects,
                               const fbx_conn_table_t *ct,
                               void *skeleton,
                               const fbx_bone_binding_t *bone_bindings,
                               int32_t bone_binding_count,
                               const fbx_mesh_binding_t *mesh_bindings,
                               int32_t mesh_binding_count,
                               const fbx_mesh_remap_t *mesh_remaps,
                               int32_t mesh_remap_count) {
    if (!objects || !ct || !skeleton || !mesh_bindings || mesh_binding_count <= 0)
        return;

    for (int32_t oi = 0; oi < objects->child_count; oi++) {
        fbx_node_t *skin = &objects->children[oi];
        int64_t skin_id;
        int64_t geometry_id;
        void *mesh_obj = NULL;
        const fbx_mesh_remap_t *remap;
        fbx_skin_influence_t *controls;
        int32_t control_count;

        if (strcmp(skin->name, "Deformer") != 0 || skin->prop_count < 3 ||
            strcmp(fbx_prop_str(skin, 2), "Skin") != 0)
            continue;

        skin_id = fbx_prop_i64(skin, 0);
        geometry_id = fbx_find_skin_geometry(objects, ct, skin_id);
        if (geometry_id == 0)
            continue;
        for (int32_t bi = 0; bi < mesh_binding_count; bi++) {
            if (mesh_bindings[bi].id == geometry_id) {
                mesh_obj = mesh_bindings[bi].mesh;
                break;
            }
        }
        if (!mesh_obj)
            continue;
        remap = fbx_find_mesh_remap(mesh_remaps, mesh_remap_count, geometry_id);
        if (!remap && ((rt_mesh3d *)mesh_obj)->vertex_count > (uint32_t)INT32_MAX)
            continue;
        control_count =
            remap ? remap->control_count : (int32_t)((rt_mesh3d *)mesh_obj)->vertex_count;
        if (control_count <= 0)
            continue;
        if (!rt_untrusted_count_ok(
                (int64_t)control_count, 3u * sizeof(double), FBX_MAX_DECOMPRESSED_ARRAY_BYTES))
            continue;
        if ((size_t)control_count > SIZE_MAX / sizeof(*controls))
            continue;
        controls = (fbx_skin_influence_t *)calloc((size_t)control_count, sizeof(*controls));
        if (!controls)
            continue;

        for (int32_t ci = fbx_connections_first_for_parent(ct, skin_id); ci >= 0;
             ci = fbx_connections_next_for_parent(ct, ci)) {
            fbx_node_t *cluster;
            int32_t bone_index;
            int64_t bone_model_id;
            uint32_t index_count = 0;
            uint32_t weight_count = 0;
            const int32_t *indices;
            const double *weights64;
            const float *weights32 = NULL;

            cluster = fbx_find_object_by_id(objects, ct->entries[ci].child_id);
            if (!cluster || strcmp(cluster->name, "Deformer") != 0 || cluster->prop_count < 3 ||
                strcmp(fbx_prop_str(cluster, 2), "Cluster") != 0)
                continue;
            bone_model_id = fbx_find_cluster_bone_model(objects, ct, fbx_prop_i64(cluster, 0));
            bone_index = fbx_find_bone_index_for_model(
                objects, skeleton, bone_bindings, bone_binding_count, bone_model_id);
            if (bone_index < 0)
                continue;

            indices = fbx_get_i32_array(cluster, "Indexes", &index_count);
            weights64 = fbx_get_f64_array(cluster, "Weights", &weight_count);
            if (!weights64)
                weights32 = fbx_get_f32_array(cluster, "Weights", &weight_count);
            if (!indices || (!weights64 && !weights32))
                continue;
            if (weight_count < index_count)
                index_count = weight_count;
            for (uint32_t wi = 0; wi < index_count; wi++) {
                int32_t control_index = indices[wi];
                double weight = weights64 ? weights64[wi] : (double)weights32[wi];
                if (control_index >= 0 && control_index < control_count)
                    fbx_skin_add_influence(&controls[control_index], bone_index, weight);
            }
        }

        fbx_apply_skin_to_mesh(mesh_obj, remap, controls, control_count);
        free(controls);
    }
}

/// @brief Borrowed, validated view over one decoded FBX animation curve.
typedef struct {
    /// Strictly increasing source key times in FBX ticks.
    const int64_t *times;
    /// Optional double-precision key values.
    const double *values64;
    /// Optional single-precision key values when `values64` is absent.
    const float *values32;
    /// Optional per-key interpolation flags.
    const int32_t *attr_flags;
    /// Optional double-precision tangent metadata.
    const double *attr_data64;
    /// Optional single-precision tangent metadata.
    const float *attr_data32;
    /// Common time/value key count.
    uint32_t count;
    /// Available interpolation-flag count.
    uint32_t attr_flag_count;
    /// Available tangent-metadata scalar count.
    uint32_t attr_data_count;
} fbx_anim_curve_view_t;

/// @brief Parsed properties and optional animated weight of one FBX layer.
typedef struct {
    /// Numeric AnimationLayer identifier.
    int64_t id;
    /// Static layer weight in percent.
    double weight;
    /// Nonzero when the layer is muted.
    int mute;
    /// Nonzero when the layer is soloed.
    int solo;
    /// FBX additive or override blend-mode selector.
    int blend_mode;
    /// Quaternion-versus-Euler rotation accumulation selector.
    int rotation_accumulation_mode;
    /// Multiplicative-versus-additive scale accumulation selector.
    int scale_accumulation_mode;
    /// Optional curve animating the layer weight.
    fbx_anim_curve_view_t weight_curve;
} fbx_anim_layer_t;

/// @brief One layer's curve-node channel targeting a model or scalar property.
typedef struct {
    /// Index into the owning stack's layer array.
    int32_t layer_index;
    /// Numeric FBX object receiving the channel.
    int64_t target_id;
    /// Borrowed connection property name.
    const char *property;
    /// Translation, rotation, or scale selector, or `-1` for a scalar property.
    int transform_path;
    /// Curve-node defaults for the three components.
    double default_values[3];
    /// Bits identifying explicitly authored defaults.
    uint8_t default_mask;
    /// Bits identifying connected valid curves.
    uint8_t curve_mask;
    /// X, Y, and Z/component curve views.
    fbx_anim_curve_view_t curves[3];
} fbx_anim_layer_channel_t;

/// @brief Flattened, load-owned representation of an FBX animation stack.
typedef struct {
    /// Owned parsed layer array.
    fbx_anim_layer_t *layers;
    /// Number of parsed layers.
    int32_t layer_count;
    /// Owned flattened curve-node channel array.
    fbx_anim_layer_channel_t *channels;
    /// Number of parsed channels.
    int32_t channel_count;
    /// Nonzero when at least one unmuted layer is soloed.
    int has_solo;
} fbx_anim_layer_stack_t;

/// Runtime selector for ordinary linear curve interpolation.
#define FBX_ANIM_INTERP_LINEAR 0
/// Runtime selector for left-key constant interpolation.
#define FBX_ANIM_INTERP_CONSTANT 1
/// Runtime selector for Hermite cubic interpolation.
#define FBX_ANIM_INTERP_CUBIC 2
/// FBX `KeyAttrFlags` bit identifying constant interpolation.
#define FBX_ANIM_INTERP_FLAG_CONSTANT 0x00000002
/// FBX `KeyAttrFlags` bit identifying linear interpolation.
#define FBX_ANIM_INTERP_FLAG_LINEAR 0x00000004
/// FBX `KeyAttrFlags` bit identifying cubic interpolation.
#define FBX_ANIM_INTERP_FLAG_CUBIC 0x00000008

/// @brief Sampling state for one runtime bone or FBX model.
typedef struct {
    /// Nonzero after static transform data has been loaded.
    int8_t initialized;
    /// Numeric FBX model identifier.
    int64_t target_id;
    /// Optional shared parsed layer stack.
    const fbx_anim_layer_stack_t *layer_stack;
    /// Static FBX transform components used for missing channels.
    fbx_transform_components_t base_transform;
    /// Legacy direct curve views indexed by TRS path and XYZ component.
    fbx_anim_curve_view_t curves[3][3]; /* trs, component */
} fbx_anim_bone_builder_t;

/// @brief One all-model animation builder used when constraints cross skeleton boundaries.
typedef struct {
    /// Numeric FBX model identifier.
    int64_t id;
    /// Numeric parent model identifier, or zero.
    int64_t parent_id;
    /// Index of the parent within the all-model array, or `-1`.
    int32_t parent_index;
    /// Original child index in the Objects node.
    int32_t import_index;
    /// Nonzero when the model represents a skeleton node.
    int is_skeleton;
    /// Borrowed source Model node.
    fbx_node_t *source;
    /// Static and animated transform sampler for the model.
    fbx_anim_bone_builder_t animation;
} fbx_anim_constraint_model_t;

/// @brief Return non-zero if @p curve contains at least one keyframe with usable value data.
/// @param[in] curve Borrowed curve view.
/// @return Nonzero when time and value storage exists and the key count is
/// within the importer ceiling.
static int fbx_anim_curve_has_data(const fbx_anim_curve_view_t *curve) {
    return curve && curve->times && (curve->values64 || curve->values32) && curve->count > 0 &&
           curve->count <= FBX_ANIM_CURVE_KEYS_MAX;
}

/// @brief Validate an FBX animation curve before the sampler assumes sorted finite data.
/// @param[in] curve Curve view to validate.
/// @return Nonzero when keys are present, times are strictly increasing and
/// bounded, and every value is finite.
static int fbx_anim_curve_view_valid(const fbx_anim_curve_view_t *curve) {
    if (!fbx_anim_curve_has_data(curve))
        return 0;
    for (uint32_t i = 0; i < curve->count; i++) {
        double value = curve->values64 ? curve->values64[i] : (double)curve->values32[i];
        double seconds = (double)curve->times[i] / (double)FBX_TIME_SECOND;
        if (!isfinite(value) || !isfinite(seconds) || fabs(seconds) > FBX_ANIM_TIME_SECONDS_MAX)
            return 0;
        if (i > 0 && curve->times[i] <= curve->times[i - 1])
            return 0;
    }
    return 1;
}

/// @brief Case-insensitive ASCII string equality (NULL operands compare unequal).
/// @param[in] a First null-terminated string.
/// @param[in] b Second null-terminated string.
/// @return Nonzero for an ASCII case-insensitive exact match, otherwise zero.
static int fbx_ascii_streq_ignore_case(const char *a, const char *b) {
    if (!a || !b)
        return 0;
    while (*a && *b) {
        unsigned char ca = (unsigned char)*a++;
        unsigned char cb = (unsigned char)*b++;
        if (tolower(ca) != tolower(cb))
            return 0;
    }
    return *a == '\0' && *b == '\0';
}

/// @brief Map an AnimationCurve connection component label to XYZ index.
/// @details Binary and ASCII FBX commonly use `d|X`, `d|Y`, `d|Z`, but some exporters
///          write the bare aliases `X`, `Y`, `Z`; others preserve the meaning but vary
///          ASCII case. Accept these forms so valid curves do not disappear solely
///          because of exporter spelling.
/// @param[in] prop Connection property label.
/// @return Zero, one, or two for X, Y, or Z; `-1` for an unsupported label.
static int32_t fbx_anim_component_from_connection_prop(const char *prop) {
    if (!prop)
        return -1;
    if (fbx_ascii_streq_ignore_case(prop, "d|X") || fbx_ascii_streq_ignore_case(prop, "X"))
        return 0;
    if (fbx_ascii_streq_ignore_case(prop, "d|Y") || fbx_ascii_streq_ignore_case(prop, "Y"))
        return 1;
    if (fbx_ascii_streq_ignore_case(prop, "d|Z") || fbx_ascii_streq_ignore_case(prop, "Z"))
        return 2;
    return -1;
}

/// @brief Return one animation curve key value as double.
/// @param curve Animation curve view.
/// @param key_index Key index to read.
/// @param fallback Returned when the key is out of range or non-finite.
/// @return The finite key value or @p fallback.
static double fbx_anim_curve_key_value(const fbx_anim_curve_view_t *curve,
                                       uint32_t key_index,
                                       double fallback) {
    double value;
    if (!fbx_anim_curve_has_data(curve) || key_index >= curve->count)
        return fallback;
    value = curve->values64 ? curve->values64[key_index] : (double)curve->values32[key_index];
    return isfinite(value) ? value : fallback;
}

/// @brief Classify an FBX curve segment's interpolation mode from `KeyAttrFlags`.
/// @details FBX SDK interpolation bits are commonly 0x2 constant, 0x4 linear, and 0x8 cubic.
///          Some exporters omit flags or write sparse metadata; those segments fall back to
///          linear interpolation because that is the safest representation for the runtime.
/// @param curve Animation curve view.
/// @param segment_index Segment starting key index.
/// @return One of FBX_ANIM_INTERP_*.
static int fbx_anim_curve_segment_mode(const fbx_anim_curve_view_t *curve, uint32_t segment_index) {
    int32_t flags;
    if (!curve || !curve->attr_flags || segment_index >= curve->attr_flag_count)
        return FBX_ANIM_INTERP_LINEAR;
    flags = curve->attr_flags[segment_index];
    if (flags & FBX_ANIM_INTERP_FLAG_CONSTANT)
        return FBX_ANIM_INTERP_CONSTANT;
    if (flags & FBX_ANIM_INTERP_FLAG_CUBIC)
        return FBX_ANIM_INTERP_CUBIC;
    if (flags & FBX_ANIM_INTERP_FLAG_LINEAR)
        return FBX_ANIM_INTERP_LINEAR;
    return FBX_ANIM_INTERP_LINEAR;
}

/// @brief Read tangent slope metadata from a curve's `KeyAttrDataFloat` array.
/// @details Exporters typically store four floats per key; this helper treats slot 0 as the
///          outgoing slope and slot 1 as the incoming slope. Two-float-per-key data is accepted
///          as a compact variant. Missing/non-finite data returns @p fallback_slope.
/// @param curve Animation curve view.
/// @param key_index Key whose tangent is requested.
/// @param incoming Non-zero for incoming tangent, zero for outgoing tangent.
/// @param fallback_slope Value used when no usable tangent metadata exists.
/// @return Tangent slope in value-per-second units.
static double fbx_anim_curve_tangent_slope(const fbx_anim_curve_view_t *curve,
                                           uint32_t key_index,
                                           int incoming,
                                           double fallback_slope) {
    uint32_t stride = 0;
    uint32_t slot;
    double value;
    if (!curve || key_index >= curve->count || curve->attr_data_count == 0 ||
        (!curve->attr_data64 && !curve->attr_data32))
        return fallback_slope;
    if (curve->attr_data_count >= curve->count * 4u)
        stride = 4u;
    else if (curve->attr_data_count >= curve->count * 2u)
        stride = 2u;
    if (stride == 0)
        return fallback_slope;
    slot = key_index * stride + (incoming ? 1u : 0u);
    if (slot >= curve->attr_data_count)
        return fallback_slope;
    value = curve->attr_data64 ? curve->attr_data64[slot] : (double)curve->attr_data32[slot];
    if (!isfinite(value))
        return fallback_slope;
    return fbx_clamp_abs_or(value, fallback_slope, FBX_NUMERIC_ABS_MAX);
}

/// @brief Hermite-sample one cubic FBX animation segment.
/// @details The runtime animation format is keyframe-only, but importing cubic source curves still
///          benefits from correct intermediate samples. Tangents are interpreted as value/second;
///          when missing, linear slope fallback makes this reduce to ordinary linear interpolation.
/// @param curve Animation curve view.
/// @param lo Segment start key index.
/// @param hi Segment end key index.
/// @param fbx_time Time inside the segment.
/// @param fallback Returned on malformed data.
/// @return Interpolated curve value.
static double fbx_anim_curve_cubic_value(const fbx_anim_curve_view_t *curve,
                                         uint32_t lo,
                                         uint32_t hi,
                                         int64_t fbx_time,
                                         double fallback) {
    int64_t t0 = curve->times[lo];
    int64_t t1 = curve->times[hi];
    double span_seconds;
    double u;
    double v0;
    double v1;
    double slope;
    double m0;
    double m1;
    double u2;
    double u3;
    double h00;
    double h10;
    double h01;
    double h11;
    if (t1 <= t0)
        return fallback;
    span_seconds = (double)(t1 - t0) / (double)FBX_TIME_SECOND;
    if (!isfinite(span_seconds) || span_seconds <= 0.0)
        return fallback;
    u = (double)(fbx_time - t0) / (double)(t1 - t0);
    if (!isfinite(u) || u < 0.0)
        u = 0.0;
    if (u > 1.0)
        u = 1.0;
    v0 = fbx_anim_curve_key_value(curve, lo, fallback);
    v1 = fbx_anim_curve_key_value(curve, hi, fallback);
    if (!isfinite(v0) || !isfinite(v1))
        return fallback;
    slope = (v1 - v0) / span_seconds;
    m0 = fbx_anim_curve_tangent_slope(curve, lo, 0, slope);
    m1 = fbx_anim_curve_tangent_slope(curve, hi, 1, slope);
    u2 = u * u;
    u3 = u2 * u;
    h00 = 2.0 * u3 - 3.0 * u2 + 1.0;
    h10 = u3 - 2.0 * u2 + u;
    h01 = -2.0 * u3 + 3.0 * u2;
    h11 = u3 - u2;
    return h00 * v0 + h10 * span_seconds * m0 + h01 * v1 + h11 * span_seconds * m1;
}

/// @brief Sample the animation curve at the given FBX tick time using FBX interpolation flags.
/// @details Times before the first keyframe return the first value; times after the last return
///          the last value (no extrapolation).  Constant segments hold the left key, cubic
///          segments use Hermite interpolation with `KeyAttrDataFloat` tangents when available,
///          and all other segments are linear. FBX time is in units of 1/46,186,158,000 second.
/// @param[in] curve Validated source curve view.
/// @param[in] fbx_time Requested source time in FBX ticks.
/// @param fallback Value returned when the curve has no data.
/// @return The sampled finite value according to the segment mode, or
/// `fallback` for unusable data.
static double fbx_anim_curve_value(const fbx_anim_curve_view_t *curve,
                                   int64_t fbx_time,
                                   double fallback) {
    uint32_t lo;
    uint32_t hi;
    int64_t t0;
    int64_t t1;
    double v0;
    double v1;
    double a;
    if (!fbx_anim_curve_has_data(curve))
        return fallback;
    v0 = fbx_anim_curve_key_value(curve, 0, fallback);
    v1 = fbx_anim_curve_key_value(curve, curve->count - 1, fallback);
    if (!isfinite(v0) || !isfinite(v1))
        return fallback;
    if (fbx_time <= curve->times[0])
        return v0;
    if (fbx_time >= curve->times[curve->count - 1])
        return v1;
    lo = 0;
    hi = curve->count - 1;
    while (hi - lo > 1) {
        uint32_t mid = lo + (hi - lo) / 2u;
        if (curve->times[mid] <= fbx_time)
            lo = mid;
        else
            hi = mid;
    }
    t0 = curve->times[lo];
    t1 = curve->times[hi];
    if (t1 <= t0)
        return fallback;
    v0 = fbx_anim_curve_key_value(curve, lo, fallback);
    v1 = fbx_anim_curve_key_value(curve, hi, fallback);
    if (!isfinite(v0) || !isfinite(v1))
        return fallback;
    {
        int mode = fbx_anim_curve_segment_mode(curve, lo);
        if (mode == FBX_ANIM_INTERP_CONSTANT)
            return v0;
        if (mode == FBX_ANIM_INTERP_CUBIC)
            return fbx_anim_curve_cubic_value(curve, lo, hi, fbx_time, fallback);
    }
    a = (double)(fbx_time - t0) / (double)(t1 - t0);
    if (!isfinite(a) || a < 0.0)
        a = 0.0;
    if (a > 1.0)
        a = 1.0;
    return v0 + (v1 - v0) * a;
}

/// @brief Build a validated borrowed view of an AnimationCurve node.
/// @param[in] curve Candidate FBX AnimationCurve node.
/// @param[out] out Destination view populated only on success.
/// @return Nonzero when key times and equally sized float or double values are
/// present, bounded, finite, and strictly ordered; otherwise zero.
/// @details Optional interpolation flags and tangent arrays are attached when
/// available but do not need to match the key count.
static int fbx_anim_read_curve_view(fbx_node_t *curve, fbx_anim_curve_view_t *out) {
    uint32_t time_count = 0;
    uint32_t value_count = 0;
    uint32_t flag_count = 0;
    uint32_t data_count = 0;
    fbx_anim_curve_view_t candidate;
    if (!curve || !out || !curve->name || strcmp(curve->name, "AnimationCurve") != 0)
        return 0;
    memset(&candidate, 0, sizeof(candidate));
    candidate.times = fbx_get_i64_array(curve, "KeyTime", &time_count);
    candidate.values64 = fbx_get_f64_array(curve, "KeyValueFloat", &value_count);
    if (!candidate.values64)
        candidate.values32 = fbx_get_f32_array(curve, "KeyValueFloat", &value_count);
    candidate.attr_flags = fbx_get_i32_array(curve, "KeyAttrFlags", &flag_count);
    candidate.attr_data64 = fbx_get_f64_array(curve, "KeyAttrDataFloat", &data_count);
    if (!candidate.attr_data64)
        candidate.attr_data32 = fbx_get_f32_array(curve, "KeyAttrDataFloat", &data_count);
    candidate.count = time_count;
    candidate.attr_flag_count = candidate.attr_flags ? flag_count : 0;
    candidate.attr_data_count = (candidate.attr_data64 || candidate.attr_data32) ? data_count : 0;
    if (time_count == 0 || time_count != value_count || !fbx_anim_curve_view_valid(&candidate))
        return 0;
    *out = candidate;
    return 1;
}

/// @brief Detect numeric curve data that must invalidate an animation graph.
/// @param[in] curve Candidate AnimationCurve node.
/// @return Nonzero when any decoded key time converts outside the permitted
/// seconds range or any decoded value is non-finite; otherwise zero.
/// @details Missing arrays are not themselves classified as unsafe here.
static int fbx_anim_curve_has_unsafe_numeric_data(fbx_node_t *curve) {
    uint32_t time_count = 0;
    uint32_t value_count = 0;
    const int64_t *times;
    const double *values64;
    const float *values32 = NULL;
    if (!curve)
        return 0;
    times = fbx_get_i64_array(curve, "KeyTime", &time_count);
    values64 = fbx_get_f64_array(curve, "KeyValueFloat", &value_count);
    if (!values64)
        values32 = fbx_get_f32_array(curve, "KeyValueFloat", &value_count);
    for (uint32_t i = 0; i < time_count; ++i) {
        double seconds = (double)times[i] / (double)FBX_TIME_SECOND;
        if (!isfinite(seconds) || fabs(seconds) > FBX_ANIM_TIME_SECONDS_MAX)
            return 1;
    }
    for (uint32_t i = 0; i < value_count; ++i) {
        double value = values64 ? values64[i] : values32 ? (double)values32[i] : 0.0;
        if (!isfinite(value))
            return 1;
    }
    return 0;
}

/// @brief Read a finite numeric layer or curve-node property with fallback.
/// @param[in] node Node searched first through `Properties70`, then direct children.
/// @param[in] name Exact property name.
/// @param[in] fallback Value returned when the property is absent.
/// @param[out] out Required destination value.
/// @param[out] out_found Optional flag set when an authored value was found.
/// @return Nonzero for an absent or finite property; zero for a missing output
/// pointer or authored non-finite value.
static int fbx_anim_layer_property_number(
    fbx_node_t *node, const char *name, double fallback, double *out, int *out_found) {
    fbx_node_t *property;
    double value;
    if (out_found)
        *out_found = 0;
    if (!out)
        return 0;
    *out = fallback;
    property = fbx_find_property70(node, name);
    if (property && property->prop_count >= 5) {
        value = fbx_prop_f64(property, 4);
        if (!isfinite(value))
            return 0;
        *out = value;
        if (out_found)
            *out_found = 1;
        return 1;
    }
    property = node ? fbx_find_child(node, name) : NULL;
    if (property && property->prop_count > 0) {
        value = fbx_prop_f64(property, 0);
        if (!isfinite(value))
            return 0;
        *out = value;
        if (out_found)
            *out_found = 1;
    }
    return 1;
}

/// @brief Map an FBX local-transform property name to a TRS path index.
/// @param[in] property Case-insensitive property name.
/// @return Zero for translation, one for rotation, two for scaling, or `-1`.
static int fbx_anim_transform_path(const char *property) {
    if (fbx_ascii_streq_ignore_case(property, "Lcl Translation"))
        return 0;
    if (fbx_ascii_streq_ignore_case(property, "Lcl Rotation"))
        return 1;
    if (fbx_ascii_streq_ignore_case(property, "Lcl Scaling"))
        return 2;
    return -1;
}

/// @brief Determine whether one parsed animation layer contributes.
/// @param[in] stack Parsed layer stack.
/// @param[in] layer_index Zero-based layer index.
/// @return Nonzero when the index is valid, the layer is not muted, and either
/// the stack has no solo layer or this layer is soloed.
static int fbx_anim_layer_is_active(const fbx_anim_layer_stack_t *stack, int32_t layer_index) {
    const fbx_anim_layer_t *layer;
    if (!stack || layer_index < 0 || layer_index >= stack->layer_count)
        return 0;
    layer = &stack->layers[layer_index];
    return !layer->mute && (!stack->has_solo || layer->solo);
}

/// @brief Free a parsed layer stack and reset it to empty.
/// @param[in,out] stack Stack owning layer and channel arrays; `NULL` is ignored.
static void fbx_anim_layer_stack_free(fbx_anim_layer_stack_t *stack) {
    if (!stack)
        return;
    free(stack->layers);
    free(stack->channels);
    memset(stack, 0, sizeof(*stack));
}

/// @brief Parse and validate all layers and curve-node channels in one stack.
/// @param[in] objects FBX Objects node.
/// @param[in] ct Indexed connection graph.
/// @param[in] stack_id Numeric AnimationStack identifier.
/// @param[in,out] context Aggregate allocation budget.
/// @param[out] out Destination stack, initialized empty before parsing.
/// @return Nonzero on success, including a stack with no layers; zero on
/// invalid graph structure, unsafe properties, budget exhaustion, or allocation
/// failure.
/// @details Layer allocation is budgeted in a counting pass. The parser
/// validates blend and accumulation modes, resolves each curve node to exactly
/// one target/property pair, records optional component defaults, rejects
/// duplicate channels, and treats a channel targeting its owning layer's
/// `Weight` property as the animated layer weight.
static int fbx_anim_layer_stack_parse(fbx_node_t *objects,
                                      const fbx_conn_table_t *ct,
                                      int64_t stack_id,
                                      fbx_load_context_t *context,
                                      fbx_anim_layer_stack_t *out) {
    int32_t layer_count = 0;
    int32_t channel_capacity = 0;
    if (!objects || !ct || !context || !out)
        return 0;
    memset(out, 0, sizeof(*out));
    for (int32_t connection = fbx_connections_first_for_parent(ct, stack_id); connection >= 0;
         connection = fbx_connections_next_for_parent(ct, connection)) {
        fbx_node_t *layer = fbx_find_object_by_id(objects, ct->entries[connection].child_id);
        if (layer && layer->name && strcmp(layer->name, "AnimationLayer") == 0) {
            if (layer_count == INT32_MAX)
                return 0;
            ++layer_count;
            if (layer->prop_count > 0) {
                int64_t layer_id = fbx_prop_i64(layer, 0);
                for (int32_t child = fbx_connections_first_for_parent(ct, layer_id); child >= 0;
                     child = fbx_connections_next_for_parent(ct, child)) {
                    fbx_node_t *curve_node =
                        fbx_find_object_by_id(objects, ct->entries[child].child_id);
                    if (curve_node && curve_node->name &&
                        strcmp(curve_node->name, "AnimationCurveNode") == 0) {
                        if (channel_capacity == INT32_MAX)
                            return 0;
                        ++channel_capacity;
                    }
                }
            }
        }
    }
    if (layer_count == 0)
        return 1;
    if ((size_t)layer_count > SIZE_MAX / sizeof(*out->layers) ||
        (size_t)channel_capacity > SIZE_MAX / sizeof(*out->channels) ||
        !fbx_budget_charge(context, (uint64_t)layer_count, sizeof(*out->layers)) ||
        !fbx_budget_charge(context, (uint64_t)channel_capacity, sizeof(*out->channels)))
        return 0;
    out->layers = (fbx_anim_layer_t *)calloc((size_t)layer_count, sizeof(*out->layers));
    out->channels =
        channel_capacity > 0
            ? (fbx_anim_layer_channel_t *)calloc((size_t)channel_capacity, sizeof(*out->channels))
            : NULL;
    if (!out->layers || (channel_capacity > 0 && !out->channels)) {
        fbx_anim_layer_stack_free(out);
        return 0;
    }
    for (int32_t connection = fbx_connections_first_for_parent(ct, stack_id); connection >= 0;
         connection = fbx_connections_next_for_parent(ct, connection)) {
        fbx_node_t *layer_node = fbx_find_object_by_id(objects, ct->entries[connection].child_id);
        fbx_anim_layer_t *layer;
        double value;
        int32_t layer_index;
        if (!layer_node || !layer_node->name || strcmp(layer_node->name, "AnimationLayer") != 0)
            continue;
        if (layer_node->prop_count < 1 || out->layer_count >= layer_count)
            goto invalid;
        layer_index = out->layer_count++;
        layer = &out->layers[layer_index];
        layer->id = fbx_prop_i64(layer_node, 0);
        if (!fbx_anim_layer_property_number(layer_node, "Weight", 100.0, &value, NULL))
            goto invalid;
        layer->weight = fbx_clamp_double(value, 0.0, 100.0, 100.0);
        if (!fbx_anim_layer_property_number(layer_node, "Mute", 0.0, &value, NULL))
            goto invalid;
        layer->mute = value != 0.0;
        if (!fbx_anim_layer_property_number(layer_node, "Solo", 0.0, &value, NULL))
            goto invalid;
        layer->solo = value != 0.0;
        if (!fbx_anim_layer_property_number(layer_node, "BlendMode", 0.0, &value, NULL) ||
            value < 0.0 || value > 2.0 || value != floor(value))
            goto invalid;
        layer->blend_mode = (int)value;
        if (!fbx_anim_layer_property_number(
                layer_node, "RotationAccumulationMode", 0.0, &value, NULL) ||
            value < 0.0 || value > 1.0 || value != floor(value))
            goto invalid;
        layer->rotation_accumulation_mode = (int)value;
        if (!fbx_anim_layer_property_number(
                layer_node, "ScaleAccumulationMode", 0.0, &value, NULL) ||
            value < 0.0 || value > 1.0 || value != floor(value))
            goto invalid;
        layer->scale_accumulation_mode = (int)value;
        if (layer->solo && !layer->mute)
            out->has_solo = 1;
        for (int32_t node_connection = fbx_connections_first_for_parent(ct, layer->id);
             node_connection >= 0;
             node_connection = fbx_connections_next_for_parent(ct, node_connection)) {
            int64_t curve_node_id = ct->entries[node_connection].child_id;
            fbx_node_t *curve_node = fbx_find_object_by_id(objects, curve_node_id);
            int64_t target_id = 0;
            const char *target_property = NULL;
            int target_count = 0;
            int already_visited = 0;
            fbx_anim_layer_channel_t candidate;
            if (!curve_node || !curve_node->name ||
                strcmp(curve_node->name, "AnimationCurveNode") != 0)
                continue;
            /* A curve node that animates an AnimationLayer property (usually Weight) has both
             * its ordinary ownership edge and an OP target edge to that same layer. Parent
             * adjacency therefore exposes the logical node twice; parse it only once while
             * retaining both edges for target resolution. */
            for (int32_t prior = fbx_connections_first_for_parent(ct, layer->id);
                 prior >= 0 && prior != node_connection;
                 prior = fbx_connections_next_for_parent(ct, prior)) {
                if (ct->entries[prior].child_id == curve_node_id) {
                    already_visited = 1;
                    break;
                }
            }
            if (already_visited)
                continue;
            memset(&candidate, 0, sizeof(candidate));
            candidate.layer_index = layer_index;
            candidate.transform_path = -1;
            for (int32_t target_connection = fbx_connections_first_for_child(ct, curve_node_id);
                 target_connection >= 0;
                 target_connection = fbx_connections_next_for_child(ct, target_connection)) {
                const char *property = ct->entries[target_connection].prop;
                if (!property || !property[0])
                    continue;
                if (target_count > 0 && (target_id != ct->entries[target_connection].parent_id ||
                                         !fbx_ascii_streq_ignore_case(target_property, property)))
                    goto invalid;
                target_id = ct->entries[target_connection].parent_id;
                target_property = property;
                ++target_count;
            }
            if (target_count == 0)
                continue;
            candidate.target_id = target_id;
            candidate.property = target_property;
            candidate.transform_path = fbx_anim_transform_path(target_property);
            for (int component = 0; component < 3; ++component) {
                static const char *names[3] = {"d|X", "d|Y", "d|Z"};
                static const char *aliases[3] = {"X", "Y", "Z"};
                double default_value = candidate.transform_path == 2 ? 1.0 : 0.0;
                int found = 0;
                if (!fbx_anim_layer_property_number(
                        curve_node, names[component], default_value, &default_value, &found))
                    goto invalid;
                if (!found &&
                    !fbx_anim_layer_property_number(
                        curve_node, aliases[component], default_value, &default_value, &found))
                    goto invalid;
                candidate.default_values[component] = default_value;
                if (found)
                    candidate.default_mask |= (uint8_t)(1u << component);
            }
            for (int32_t curve_connection = fbx_connections_first_for_parent(ct, curve_node_id);
                 curve_connection >= 0;
                 curve_connection = fbx_connections_next_for_parent(ct, curve_connection)) {
                fbx_node_t *curve =
                    fbx_find_object_by_id(objects, ct->entries[curve_connection].child_id);
                int component;
                if (!curve || !curve->name || strcmp(curve->name, "AnimationCurve") != 0)
                    continue;
                component =
                    fbx_anim_component_from_connection_prop(ct->entries[curve_connection].prop);
                if (component < 0 && candidate.transform_path < 0)
                    component = 0;
                if (component < 0 || component > 2)
                    goto invalid;
                if (candidate.curve_mask & (uint8_t)(1u << component))
                    continue;
                if (!fbx_anim_read_curve_view(curve, &candidate.curves[component])) {
                    if (fbx_anim_curve_has_unsafe_numeric_data(curve))
                        goto invalid;
                    continue;
                }
                candidate.curve_mask |= (uint8_t)(1u << component);
            }
            if (target_id == layer->id && fbx_ascii_streq_ignore_case(target_property, "Weight")) {
                if ((candidate.curve_mask & 1u) == 0 ||
                    fbx_anim_curve_has_data(&layer->weight_curve))
                    goto invalid;
                layer->weight_curve = candidate.curves[0];
                continue;
            }
            for (int32_t existing = 0; existing < out->channel_count; ++existing) {
                const fbx_anim_layer_channel_t *prior = &out->channels[existing];
                if (prior->layer_index == candidate.layer_index &&
                    prior->target_id == candidate.target_id &&
                    fbx_ascii_streq_ignore_case(prior->property, candidate.property))
                    goto invalid;
            }
            if (out->channel_count >= channel_capacity)
                goto invalid;
            out->channels[out->channel_count++] = candidate;
        }
    }
    return 1;
invalid:
    rt_asset_error_set_if_empty(RT_ASSET_ERROR_CORRUPT,
                                "FBX.Load: invalid animation layer graph or properties");
    fbx_anim_layer_stack_free(out);
    return 0;
}

/// @brief Sample one layer's effective normalized weight.
/// @param[in] layer Parsed layer.
/// @param[in] time Source time in FBX ticks.
/// @return The static or animated percentage clamped to `[0, 100]` and divided
/// by 100, or zero for a null layer.
static double fbx_anim_layer_weight_at(const fbx_anim_layer_t *layer, int64_t time) {
    double percent;
    if (!layer)
        return 0.0;
    percent = fbx_anim_curve_value(&layer->weight_curve, time, layer->weight);
    return fbx_clamp_double(percent, 0.0, 100.0, layer->weight) / 100.0;
}

/// @brief Find a layer channel for one target and TRS path.
/// @param[in] stack Parsed layer stack.
/// @param[in] layer_index Owning layer index.
/// @param[in] target_id Numeric target model identifier.
/// @param[in] transform_path Translation, rotation, or scale index.
/// @return The first borrowed exact-match channel, or `NULL`.
static const fbx_anim_layer_channel_t *fbx_anim_layer_find_channel(
    const fbx_anim_layer_stack_t *stack,
    int32_t layer_index,
    int64_t target_id,
    int transform_path) {
    if (!stack)
        return NULL;
    for (int32_t i = 0; i < stack->channel_count; ++i) {
        const fbx_anim_layer_channel_t *channel = &stack->channels[i];
        if (channel->layer_index == layer_index && channel->target_id == target_id &&
            channel->transform_path == transform_path)
            return channel;
    }
    return NULL;
}

/// @brief Sample one component of a parsed layer channel.
/// @param[in] channel Channel to sample.
/// @param[in] component Component index from zero through two.
/// @param[in] time Source time in FBX ticks.
/// @param[in] fallback Value used when no component default is present.
/// @param[out] out Destination sampled value.
/// @param[out] out_present Optional flag indicating an authored default or curve.
/// @return Nonzero when arguments are valid and the result is finite; otherwise
/// zero.
static int fbx_anim_layer_channel_value(const fbx_anim_layer_channel_t *channel,
                                        int component,
                                        int64_t time,
                                        double fallback,
                                        double *out,
                                        int *out_present) {
    uint8_t bit = (uint8_t)(1u << component);
    int present;
    if (!channel || component < 0 || component > 2 || !out)
        return 0;
    present = (channel->curve_mask & bit) != 0 || (channel->default_mask & bit) != 0;
    *out = (channel->default_mask & bit) != 0 ? channel->default_values[component] : fallback;
    if (channel->curve_mask & bit)
        *out = fbx_anim_curve_value(&channel->curves[component], time, *out);
    if (!isfinite(*out))
        return 0;
    if (out_present)
        *out_present = present;
    return 1;
}

/// @brief Convert FBX Euler degrees and rotation order to a quaternion.
/// @param[in] euler Three Euler angles in degrees.
/// @param[in] rotation_order FBX rotation-order selector.
/// @param[out] quaternion Destination `(x, y, z, w)` quaternion.
static void fbx_anim_quaternion_from_euler_order(const double euler[3],
                                                 int rotation_order,
                                                 double quaternion[4]) {
    double matrix[16];
    fbx_mat4_euler_degrees(euler, rotation_order, matrix);
    fbx_decompose_matrix_trs(matrix, NULL, quaternion, NULL);
}

/// @brief Compose all active layers into one model's transform components.
/// @param[in] stack Parsed layer stack, or `NULL` for the base transform.
/// @param[in] target_id Numeric target model identifier.
/// @param[in] base Static transform and metadata used as the starting value.
/// @param[in] time Source time in FBX ticks.
/// @param[out] out Fully composed and sanitized transform components.
/// @return Nonzero on success, or zero for invalid inputs, non-finite samples,
/// or invalid multiplicative-scale values.
/// @details The first layer is treated as override. Later layers follow their
/// additive/override, Euler/quaternion rotation, and multiplicative/additive
/// scale accumulation settings. Mute and solo state and animated layer weights
/// are honored.
static int fbx_anim_layer_sample_components(const fbx_anim_layer_stack_t *stack,
                                            int64_t target_id,
                                            const fbx_transform_components_t *base,
                                            int64_t time,
                                            fbx_transform_components_t *out) {
    double accumulated_quaternion[4];
    double accumulated_euler[3];
    int euler_valid = 1;
    if (!base || !out)
        return 0;
    *out = *base;
    memcpy(accumulated_euler, base->rotation, sizeof(accumulated_euler));
    fbx_anim_quaternion_from_euler_order(
        accumulated_euler, base->rotation_order, accumulated_quaternion);
    if (!stack || stack->layer_count == 0)
        return 1;
    for (int32_t layer_index = 0; layer_index < stack->layer_count; ++layer_index) {
        const fbx_anim_layer_t *layer = &stack->layers[layer_index];
        const fbx_anim_layer_channel_t *translation;
        const fbx_anim_layer_channel_t *rotation;
        const fbx_anim_layer_channel_t *scale;
        double weight;
        int blend_mode;
        if (!fbx_anim_layer_is_active(stack, layer_index))
            continue;
        weight = fbx_anim_layer_weight_at(layer, time);
        if (weight <= 0.0)
            continue;
        blend_mode = layer_index == 0 ? 1 : layer->blend_mode;
        translation = fbx_anim_layer_find_channel(stack, layer_index, target_id, 0);
        rotation = fbx_anim_layer_find_channel(stack, layer_index, target_id, 1);
        scale = fbx_anim_layer_find_channel(stack, layer_index, target_id, 2);
        if (translation) {
            for (int component = 0; component < 3; ++component) {
                double value;
                int present;
                if (!fbx_anim_layer_channel_value(translation,
                                                  component,
                                                  time,
                                                  base->translation[component],
                                                  &value,
                                                  &present))
                    return 0;
                if (blend_mode == 1)
                    present = 1;
                if (!present)
                    continue;
                if (blend_mode == 0)
                    out->translation[component] += value * weight;
                else
                    out->translation[component] += (value - out->translation[component]) * weight;
            }
        }
        if (scale) {
            for (int component = 0; component < 3; ++component) {
                double value;
                int present;
                if (!fbx_anim_layer_channel_value(
                        scale, component, time, base->scale[component], &value, &present))
                    return 0;
                if (blend_mode == 1)
                    present = 1;
                if (!present)
                    continue;
                if (blend_mode == 0 && layer->scale_accumulation_mode == 0) {
                    double factor;
                    if (!(value > 0.0))
                        return 0;
                    factor = pow(value, weight);
                    if (!isfinite(factor))
                        return 0;
                    out->scale[component] *= factor;
                } else if (blend_mode == 0) {
                    out->scale[component] += (value - 1.0) * weight;
                } else {
                    out->scale[component] += (value - out->scale[component]) * weight;
                }
            }
        }
        if (rotation) {
            double layer_euler[3];
            int any_present = 0;
            for (int component = 0; component < 3; ++component) {
                double fallback = blend_mode == 0 ? 0.0 : base->rotation[component];
                int present;
                if (!fbx_anim_layer_channel_value(
                        rotation, component, time, fallback, &layer_euler[component], &present))
                    return 0;
                if (blend_mode == 1)
                    present = 1;
                if (!present)
                    layer_euler[component] = fallback;
                else
                    any_present = 1;
            }
            if (!any_present && blend_mode != 1)
                continue;
            if (layer->rotation_accumulation_mode == 1) {
                if (!euler_valid)
                    fbx_constraint_euler_xyz_from_quaternion(accumulated_quaternion,
                                                             accumulated_euler);
                for (int component = 0; component < 3; ++component) {
                    uint8_t bit = (uint8_t)(1u << component);
                    int present = (rotation->curve_mask & bit) != 0 ||
                                  (rotation->default_mask & bit) != 0 || blend_mode == 1;
                    if (!present)
                        continue;
                    if (blend_mode == 0)
                        accumulated_euler[component] += layer_euler[component] * weight;
                    else
                        accumulated_euler[component] +=
                            (layer_euler[component] - accumulated_euler[component]) * weight;
                    accumulated_euler[component] =
                        fbx_sanitize_rotation_degrees(accumulated_euler[component]);
                }
                fbx_anim_quaternion_from_euler_order(
                    accumulated_euler, base->rotation_order, accumulated_quaternion);
                euler_valid = 1;
            } else {
                double layer_quaternion[4];
                fbx_anim_quaternion_from_euler_order(
                    layer_euler, base->rotation_order, layer_quaternion);
                if (blend_mode == 0) {
                    static const double identity[4] = {0.0, 0.0, 0.0, 1.0};
                    double weighted[4];
                    fbx_constraint_quaternion_nlerp(identity, layer_quaternion, weight, weighted);
                    fbx_constraint_quaternion_multiply(
                        accumulated_quaternion, weighted, accumulated_quaternion);
                } else {
                    fbx_constraint_quaternion_nlerp(
                        accumulated_quaternion, layer_quaternion, weight, accumulated_quaternion);
                }
                euler_valid = 0;
            }
        }
    }
    for (int component = 0; component < 3; ++component) {
        out->translation[component] =
            fbx_clamp_abs_or(out->translation[component], 0.0, FBX_NUMERIC_ABS_MAX);
        out->scale[component] = fbx_scale_or_unit(out->scale[component]);
        if (euler_valid)
            out->rotation[component] = accumulated_euler[component];
    }
    memcpy(out->sampled_rotation_quaternion,
           accumulated_quaternion,
           sizeof(out->sampled_rotation_quaternion));
    out->use_sampled_rotation_quaternion = 1;
    return 1;
}

/// @brief Compose active layer channels for one scalar target property.
/// @param[in] stack Parsed layer stack, or `NULL`.
/// @param[in] target_id Numeric target object identifier.
/// @param[in] property Optional case-insensitive property filter; `NULL`
/// selects only non-transform channels.
/// @param[in] base Starting scalar value and missing-component fallback.
/// @param[in] time Source time in FBX ticks.
/// @param[out] out Destination composed finite scalar.
/// @param[out] out_found Optional flag indicating that a contributing channel
/// matched.
/// @return Nonzero on success, otherwise zero.
static int fbx_anim_layer_sample_scalar(const fbx_anim_layer_stack_t *stack,
                                        int64_t target_id,
                                        const char *property,
                                        double base,
                                        int64_t time,
                                        double *out,
                                        int *out_found) {
    double value = base;
    int found = 0;
    if (!out)
        return 0;
    if (stack)
        for (int32_t layer_index = 0; layer_index < stack->layer_count; ++layer_index) {
            const fbx_anim_layer_t *layer = &stack->layers[layer_index];
            double weight;
            int blend_mode;
            if (!fbx_anim_layer_is_active(stack, layer_index))
                continue;
            weight = fbx_anim_layer_weight_at(layer, time);
            if (weight <= 0.0)
                continue;
            blend_mode = layer_index == 0 ? 1 : layer->blend_mode;
            for (int32_t channel_index = 0; channel_index < stack->channel_count; ++channel_index) {
                const fbx_anim_layer_channel_t *channel = &stack->channels[channel_index];
                double layer_value;
                int present;
                if (channel->layer_index != layer_index || channel->target_id != target_id ||
                    (property && !fbx_ascii_streq_ignore_case(channel->property, property)) ||
                    (!property && channel->transform_path >= 0))
                    continue;
                if (!fbx_anim_layer_channel_value(channel, 0, time, base, &layer_value, &present))
                    return 0;
                if (!present && blend_mode != 1)
                    continue;
                found = 1;
                if (blend_mode == 0)
                    value += layer_value * weight;
                else
                    value += (layer_value - value) * weight;
            }
        }
    if (!isfinite(value))
        return 0;
    *out = value;
    if (out_found)
        *out_found = found;
    return 1;
}

/// @brief qsort comparator for int64 keys, ascending (returns -1/0/1).
/// @param[in] a Pointer to the first `int64_t`.
/// @param[in] b Pointer to the second `int64_t`.
/// @return Negative, zero, or positive when the first value is less than,
/// equal to, or greater than the second.
static int fbx_i64_compare(const void *a, const void *b) {
    int64_t av = *(const int64_t *)a;
    int64_t bv = *(const int64_t *)b;
    return (av > bv) - (av < bv);
}

/// @brief Append @p value to an unsorted key-time array; sorting/dedup happens once after collect.
/// @details Non-finite or extreme key times indicate a corrupt animation curve and are reported as
///          failure so callers can reject the stack instead of silently producing an empty clip.
/// @param[in,out] times Address of the owned dynamic tick array.
/// @param[in,out] count Address of its logical count.
/// @param[in,out] capacity Address of its allocated capacity.
/// @param[in] value FBX tick value to append.
/// @param[in,out] context Aggregate allocation budget charged for growth.
/// @return Nonzero on success, or zero for invalid metadata, an unsafe time,
/// the runtime key ceiling, budget exhaustion, overflow, or allocation failure.
static int fbx_anim_append_time(int64_t **times,
                                int32_t *count,
                                int32_t *capacity,
                                int64_t value,
                                fbx_load_context_t *context) {
    double seconds = (double)value / (double)FBX_TIME_SECOND;
    if (!times || !count || !capacity || !context || *count < 0 || *capacity < 0 ||
        *count > *capacity || (*count > 0 && !*times) ||
        *count >= RT_ANIMATION3D_MAX_KEYFRAMES_PER_CHANNEL)
        return 0;
    if (!isfinite(seconds) || fabs(seconds) > FBX_ANIM_TIME_SECONDS_MAX)
        return 0;
    if (*count >= *capacity) {
        int32_t new_capacity;
        if (*capacity > INT32_MAX / 2)
            new_capacity = *count + 1;
        else
            new_capacity = *capacity == 0 ? 16 : *capacity * 2;
        if (new_capacity > RT_ANIMATION3D_MAX_KEYFRAMES_PER_CHANNEL)
            new_capacity = RT_ANIMATION3D_MAX_KEYFRAMES_PER_CHANNEL;
        if (new_capacity <= *capacity || (size_t)new_capacity > SIZE_MAX / sizeof(**times) ||
            !fbx_budget_charge(context, (uint64_t)(new_capacity - *capacity), sizeof(**times)))
            return 0;
        int64_t *grown = (int64_t *)realloc(*times, (size_t)new_capacity * sizeof(*grown));
        if (!grown)
            return 0;
        *times = grown;
        *capacity = new_capacity;
    }
    (*times)[*count] = value;
    (*count)++;
    return 1;
}

/// @brief Test whether one cubic sub-interval is sufficiently represented by a straight chord.
/// @details The authored Hermite curve is evaluated at quarter, midpoint, and three-quarter ticks.
///          Each sample is compared with linear interpolation between the sub-interval endpoints
///          using the mixed bound `1e-4 + 1e-4 * max(abs(value))`. A sub-interval must also be no
///          longer than one millisecond before it can terminate subdivision.
/// @param curve Source curve.
/// @param segment_lo Authored segment's starting key index.
/// @param segment_hi Authored segment's ending key index.
/// @param time_lo Current sub-interval start tick.
/// @param time_hi Current sub-interval end tick.
/// @return Nonzero when both time-span and value-error bounds are satisfied.
static int fbx_anim_cubic_interval_satisfies_bounds(const fbx_anim_curve_view_t *curve,
                                                    uint32_t segment_lo,
                                                    uint32_t segment_hi,
                                                    int64_t time_lo,
                                                    int64_t time_hi) {
    static const int fractions[3] = {1, 2, 3};
    int64_t delta = time_hi - time_lo;
    int64_t max_time_ticks = (FBX_TIME_SECOND + 999) / 1000;
    double value_lo;
    double value_hi;
    if (delta <= 1)
        return 1;
    if (delta > max_time_ticks)
        return 0;
    value_lo = fbx_anim_curve_cubic_value(curve, segment_lo, segment_hi, time_lo, 0.0);
    value_hi = fbx_anim_curve_cubic_value(curve, segment_lo, segment_hi, time_hi, value_lo);
    if (!isfinite(value_lo) || !isfinite(value_hi))
        return 0;
    for (int i = 0; i < 3; i++) {
        int64_t sample_time = time_lo + (delta * fractions[i]) / 4;
        double fraction;
        double exact;
        double linear;
        double magnitude;
        double tolerance;
        if (sample_time <= time_lo || sample_time >= time_hi)
            continue;
        fraction = (double)(sample_time - time_lo) / (double)delta;
        exact = fbx_anim_curve_cubic_value(curve, segment_lo, segment_hi, sample_time, value_lo);
        linear = value_lo + (value_hi - value_lo) * fraction;
        magnitude = fmax(fmax(fabs(value_lo), fabs(value_hi)), fmax(fabs(exact), fabs(linear)));
        tolerance = 1e-4 + 1e-4 * magnitude;
        if (!isfinite(exact) || !isfinite(linear) || fabs(exact - linear) > tolerance)
            return 0;
    }
    return 1;
}

/// @brief Recursively add the minimum tick samples needed for one cubic Hermite interval.
/// @details Subdivision stops only after @ref fbx_anim_cubic_interval_satisfies_bounds succeeds.
///          Every midpoint is representable, budgeted, and bounded by the runtime channel ceiling;
///          failure to meet those constraints propagates so the entire FBX load can roll back.
/// @param times Dynamic unsorted sample-time array.
/// @param count Current array element count.
/// @param capacity Current array capacity.
/// @param curve Source curve.
/// @param segment_lo Authored segment start key index.
/// @param segment_hi Authored segment end key index.
/// @param time_lo Current sub-interval start tick.
/// @param time_hi Current sub-interval end tick.
/// @param depth Recursion depth guard.
/// @param context Active aggregate load budget.
/// @return Nonzero on success, zero when bounds cannot be met safely.
static int fbx_anim_append_adaptive_cubic_times(int64_t **times,
                                                int32_t *count,
                                                int32_t *capacity,
                                                const fbx_anim_curve_view_t *curve,
                                                uint32_t segment_lo,
                                                uint32_t segment_hi,
                                                int64_t time_lo,
                                                int64_t time_hi,
                                                int depth,
                                                fbx_load_context_t *context) {
    int64_t midpoint;
    if (fbx_anim_cubic_interval_satisfies_bounds(curve, segment_lo, segment_hi, time_lo, time_hi)) {
        return 1;
    }
    if (depth >= 63 || time_hi - time_lo <= 1)
        return 0;
    midpoint = time_lo + (time_hi - time_lo) / 2;
    if (midpoint <= time_lo || midpoint >= time_hi ||
        !fbx_anim_append_time(times, count, capacity, midpoint, context)) {
        return 0;
    }
    return fbx_anim_append_adaptive_cubic_times(times,
                                                count,
                                                capacity,
                                                curve,
                                                segment_lo,
                                                segment_hi,
                                                time_lo,
                                                midpoint,
                                                depth + 1,
                                                context) &&
           fbx_anim_append_adaptive_cubic_times(times,
                                                count,
                                                capacity,
                                                curve,
                                                segment_lo,
                                                segment_hi,
                                                midpoint,
                                                time_hi,
                                                depth + 1,
                                                context);
}

/// @brief Append all runtime-needed sample times for an FBX animation curve.
/// @details Adds authored key times unconditionally. Constant segments also add one representable
///          immediately preceding FBX tick so runtime linear interpolation holds the previous value
///          through the final representable instant. Cubic segments are recursively subdivided to
///          the documented one-millisecond and mixed absolute/relative error bounds.
/// @param times Dynamic array receiving unsorted FBX tick times.
/// @param count Current array count.
/// @param capacity Current array capacity.
/// @param curve Curve whose key and intermediate sample times should be appended.
/// @param context Aggregate allocation budget charged for appended times.
/// @return Non-zero on success.
static int fbx_anim_append_curve_sample_times(int64_t **times,
                                              int32_t *count,
                                              int32_t *capacity,
                                              const fbx_anim_curve_view_t *curve,
                                              fbx_load_context_t *context) {
    if (!fbx_anim_curve_has_data(curve))
        return 1;
    for (uint32_t k = 0; k < curve->count; k++) {
        if (!fbx_anim_append_time(times, count, capacity, curve->times[k], context))
            return 0;
    }
    for (uint32_t k = 0; k + 1u < curve->count; k++) {
        int64_t t0 = curve->times[k];
        int64_t t1 = curve->times[k + 1u];
        int64_t delta = t1 - t0;
        int mode;
        if (delta <= 1)
            continue;
        mode = fbx_anim_curve_segment_mode(curve, k);
        if (mode == FBX_ANIM_INTERP_CONSTANT) {
            if (t1 - 1 > t0 && !fbx_anim_append_time(times, count, capacity, t1 - 1, context))
                return 0;
        } else if (mode == FBX_ANIM_INTERP_CUBIC &&
                   !fbx_anim_append_adaptive_cubic_times(
                       times, count, capacity, curve, k, k + 1u, t0, t1, 0, context)) {
            return 0;
        }
    }
    return 1;
}

/// @brief Append sample times from selected active channels and layer weights.
/// @param[in,out] times Address of the unsorted dynamic tick array.
/// @param[in,out] count Address of its logical count.
/// @param[in,out] capacity Address of its allocated capacity.
/// @param[in] stack Parsed layer stack; `NULL` contributes no times.
/// @param[in] target_id Target object filter, or zero to accept all targets.
/// @param[in] transform_only Nonzero to exclude scalar property channels.
/// @param[in] property Optional case-insensitive property filter.
/// @param[in,out] context Aggregate allocation budget.
/// @return Nonzero when every selected key, constant boundary, cubic adaptive
/// sample, and relevant animated layer-weight time is appended.
static int fbx_anim_layer_stack_append_times(int64_t **times,
                                             int32_t *count,
                                             int32_t *capacity,
                                             const fbx_anim_layer_stack_t *stack,
                                             int64_t target_id,
                                             int transform_only,
                                             const char *property,
                                             fbx_load_context_t *context) {
    if (!stack)
        return 1;
    for (int32_t layer_index = 0; layer_index < stack->layer_count; ++layer_index) {
        int layer_used = 0;
        if (!fbx_anim_layer_is_active(stack, layer_index))
            continue;
        for (int32_t channel_index = 0; channel_index < stack->channel_count; ++channel_index) {
            const fbx_anim_layer_channel_t *channel = &stack->channels[channel_index];
            if (channel->layer_index != layer_index ||
                (target_id != 0 && channel->target_id != target_id) ||
                (transform_only && channel->transform_path < 0) ||
                (property && !fbx_ascii_streq_ignore_case(channel->property, property)))
                continue;
            layer_used = 1;
            for (int component = 0; component < 3; ++component)
                if (!fbx_anim_append_curve_sample_times(
                        times, count, capacity, &channel->curves[component], context))
                    return 0;
        }
        if (layer_used &&
            !fbx_anim_append_curve_sample_times(
                times, count, capacity, &stack->layers[layer_index].weight_curve, context))
            return 0;
    }
    return 1;
}

/// @brief Find the source-time extent of active channels in a layer stack.
/// @param[in] stack Parsed layer stack.
/// @param[in] transform_only Nonzero to ignore scalar property channels.
/// @param[out] minimum Destination earliest FBX tick.
/// @param[out] maximum Destination latest FBX tick.
/// @return Nonzero when at least one selected channel or relevant animated
/// layer weight has curve data; otherwise zero.
static int fbx_anim_layer_stack_bounds(const fbx_anim_layer_stack_t *stack,
                                       int transform_only,
                                       int64_t *minimum,
                                       int64_t *maximum) {
    int found = 0;
    if (!stack || !minimum || !maximum)
        return 0;
    for (int32_t layer_index = 0; layer_index < stack->layer_count; ++layer_index) {
        int layer_used = 0;
        if (!fbx_anim_layer_is_active(stack, layer_index))
            continue;
        for (int32_t channel_index = 0; channel_index < stack->channel_count; ++channel_index) {
            const fbx_anim_layer_channel_t *channel = &stack->channels[channel_index];
            if (channel->layer_index != layer_index ||
                (transform_only && channel->transform_path < 0))
                continue;
            if (channel->curve_mask != 0 || channel->default_mask != 0)
                layer_used = 1;
            for (int component = 0; component < 3; ++component) {
                const fbx_anim_curve_view_t *curve = &channel->curves[component];
                if (!fbx_anim_curve_has_data(curve))
                    continue;
                if (!found || curve->times[0] < *minimum)
                    *minimum = curve->times[0];
                if (!found || curve->times[curve->count - 1] > *maximum)
                    *maximum = curve->times[curve->count - 1];
                found = 1;
            }
        }
        if (layer_used && fbx_anim_curve_has_data(&stack->layers[layer_index].weight_curve)) {
            const fbx_anim_curve_view_t *weight = &stack->layers[layer_index].weight_curve;
            if (!found || weight->times[0] < *minimum)
                *minimum = weight->times[0];
            if (!found || weight->times[weight->count - 1] > *maximum)
                *maximum = weight->times[weight->count - 1];
            found = 1;
        }
    }
    return found;
}

/// @brief Sort key times and compact duplicates in place.
/// @param[in,out] times Tick array to reorder and compact.
/// @param[in,out] count Logical count, replaced with the unique count.
static void fbx_anim_sort_unique_times(int64_t *times, int32_t *count) {
    int32_t write = 0;
    if (!times || !count || *count <= 1)
        return;
    qsort(times, (size_t)*count, sizeof(*times), fbx_i64_compare);
    for (int32_t read = 0; read < *count; ++read) {
        if (write == 0 || times[read] != times[write - 1])
            times[write++] = times[read];
    }
    *count = write;
}

/// @brief Initialize per-bone samplers for one FBX animation stack.
/// @param[in] objects FBX Objects node.
/// @param[in] ct Indexed connection graph.
/// @param[in] skeleton Target runtime skeleton.
/// @param[in] bone_bindings Numeric model-to-bone mappings.
/// @param[in] bone_binding_count Number of bone mappings.
/// @param[in] bone_count Number of builder slots and runtime bones.
/// @param[in] stack_id Numeric AnimationStack identifier.
/// @param[in] layer_stack Parsed stack shared by every builder, or `NULL` to
/// use the legacy direct-curve representation.
/// @param[out] builders Zero-initialized per-bone builder array.
/// @details Every mapped bone receives its static transform components and
/// numeric target ID. With a parsed layer stack, sampling resolves channels
/// from that shared representation. The legacy path instead traverses layer,
/// curve-node, model, and component connections and stores the first valid
/// direct curve in each bone/TRS/component slot.
static void fbx_anim_collect_curves(fbx_node_t *objects,
                                    const fbx_conn_table_t *ct,
                                    void *skeleton,
                                    const fbx_bone_binding_t *bone_bindings,
                                    int32_t bone_binding_count,
                                    int64_t bone_count,
                                    int64_t stack_id,
                                    const fbx_anim_layer_stack_t *layer_stack,
                                    fbx_anim_bone_builder_t *builders) {
    if (!objects || !ct || !builders)
        return;
    for (int32_t binding_index = 0; binding_index < bone_binding_count; binding_index++) {
        int32_t bone_index = bone_bindings[binding_index].bone_index;
        fbx_node_t *model_node;
        if (bone_index < 0 || (int64_t)bone_index >= bone_count)
            continue;
        model_node = fbx_find_object_by_id(objects, bone_bindings[binding_index].model_id);
        if (!model_node || !model_node->name || strcmp(model_node->name, "Model") != 0)
            continue;
        fbx_read_model_transform_components(model_node, &builders[bone_index].base_transform);
        builders[bone_index].target_id = bone_bindings[binding_index].model_id;
        builders[bone_index].layer_stack = layer_stack;
        builders[bone_index].initialized = 1;
    }
    if (layer_stack)
        return;
    for (int32_t li = fbx_connections_first_for_parent(ct, stack_id); li >= 0;
         li = fbx_connections_next_for_parent(ct, li)) {
        int64_t layer_id;
        fbx_node_t *layer_node;
        layer_id = ct->entries[li].child_id;
        layer_node = fbx_find_object_by_id(objects, layer_id);
        if (!layer_node || strcmp(layer_node->name, "AnimationLayer") != 0)
            continue;

        for (int32_t ci = fbx_connections_first_for_parent(ct, layer_id); ci >= 0;
             ci = fbx_connections_next_for_parent(ct, ci)) {
            int64_t curve_node_id;
            fbx_node_t *cn_node;
            curve_node_id = ct->entries[ci].child_id;
            cn_node = fbx_find_object_by_id(objects, curve_node_id);
            if (!cn_node || strcmp(cn_node->name, "AnimationCurveNode") != 0)
                continue;

            /* Find which Model (bone) this curve node connects to and what property */
            /* The CurveNode→Model connection has prop "Lcl Translation"/"Lcl Rotation"/"Lcl
             * Scaling" */
            int32_t trs_type = -1; /* 0=T, 1=R, 2=S */
            int64_t model_id = 0;
            for (int32_t ci2 = fbx_connections_first_for_child(ct, curve_node_id); ci2 >= 0;
                 ci2 = fbx_connections_next_for_child(ct, ci2)) {
                int64_t pid = ct->entries[ci2].parent_id;
                fbx_node_t *pnode = fbx_find_object_by_id(objects, pid);
                if (pnode && strcmp(pnode->name, "Model") == 0) {
                    model_id = pid;
                    const char *cprop = ct->entries[ci2].prop;
                    if (!cprop)
                        continue;
                    if (strcmp(cprop, "Lcl Translation") == 0)
                        trs_type = 0;
                    else if (strcmp(cprop, "Lcl Rotation") == 0)
                        trs_type = 1;
                    else if (strcmp(cprop, "Lcl Scaling") == 0)
                        trs_type = 2;
                    break;
                }
            }
            if (trs_type < 0 || model_id == 0)
                continue;

            int64_t bone_idx = fbx_find_bone_index_for_model(
                objects, skeleton, bone_bindings, bone_binding_count, model_id);
            if (bone_idx < 0 || bone_idx >= bone_count)
                continue;
            if (!builders[bone_idx].initialized) {
                fbx_node_t *model_node = fbx_find_object_by_id(objects, model_id);
                fbx_read_model_transform_components(model_node, &builders[bone_idx].base_transform);
                builders[bone_idx].initialized = 1;
            }

            /* Find AnimationCurve children (d|X, d|Y, d|Z) */
            for (int32_t ki = fbx_connections_first_for_parent(ct, curve_node_id); ki >= 0;
                 ki = fbx_connections_next_for_parent(ct, ki)) {
                fbx_node_t *curve;
                const char *curve_prop;
                curve = fbx_find_object_by_id(objects, ct->entries[ki].child_id);
                if (!curve || strcmp(curve->name, "AnimationCurve") != 0)
                    continue;

                curve_prop = ct->entries[ki].prop;
                int comp = fbx_anim_component_from_connection_prop(curve_prop);
                if (comp < 0)
                    continue;

                uint32_t tc = 0;
                const int64_t *times = fbx_get_i64_array(curve, "KeyTime", &tc);
                if (!times || tc == 0)
                    continue;

                uint32_t vc = 0;
                const double *dvals = fbx_get_f64_array(curve, "KeyValueFloat", &vc);
                const float *fvals = NULL;
                if (!dvals)
                    fvals = fbx_get_f32_array(curve, "KeyValueFloat", &vc);
                if (!dvals && !fvals)
                    continue;
                if (tc != vc)
                    continue;

                {
                    fbx_anim_curve_view_t *view = &builders[bone_idx].curves[trs_type][comp];
                    fbx_anim_curve_view_t candidate;
                    uint32_t attr_flag_count = 0;
                    uint32_t attr_data_count = 0;
                    const int32_t *attr_flags =
                        fbx_get_i32_array(curve, "KeyAttrFlags", &attr_flag_count);
                    const double *attr_data64 =
                        fbx_get_f64_array(curve, "KeyAttrDataFloat", &attr_data_count);
                    const float *attr_data32 = NULL;
                    if (fbx_anim_curve_has_data(view))
                        continue;
                    if (!attr_data64)
                        attr_data32 =
                            fbx_get_f32_array(curve, "KeyAttrDataFloat", &attr_data_count);
                    memset(&candidate, 0, sizeof(candidate));
                    candidate.times = times;
                    candidate.values64 = dvals;
                    candidate.values32 = fvals;
                    candidate.attr_flags = attr_flags;
                    candidate.attr_data64 = attr_data64;
                    candidate.attr_data32 = attr_data32;
                    candidate.count = tc;
                    candidate.attr_flag_count = attr_flags ? attr_flag_count : 0;
                    candidate.attr_data_count = (attr_data64 || attr_data32) ? attr_data_count : 0;
                    if (fbx_anim_curve_view_valid(&candidate))
                        *view = candidate;
                }
            }
        }
    }
}

/// @brief True if the given FBX animation stack id owns at least one AnimationLayer child in
///   the connection table.
/// @param[in] objects FBX Objects node.
/// @param[in] ct Indexed connection graph.
/// @param[in] stack_id Numeric AnimationStack identifier.
/// @return Nonzero when a connected child resolves to an AnimationLayer.
static int fbx_anim_stack_has_layer(fbx_node_t *objects,
                                    const fbx_conn_table_t *ct,
                                    int64_t stack_id) {
    if (!objects || !ct)
        return 0;
    for (int32_t i = fbx_connections_first_for_parent(ct, stack_id); i >= 0;
         i = fbx_connections_next_for_parent(ct, i)) {
        fbx_node_t *layer_node;
        layer_node = fbx_find_object_by_id(objects, ct->entries[i].child_id);
        if (layer_node && strcmp(layer_node->name, "AnimationLayer") == 0)
            return 1;
    }
    return 0;
}

/// @brief Smallest and largest FBX key ticks across all of @p builders' curves.
/// @param[in] builders Per-bone sampler array.
/// @param[in] bone_count Number of builder entries.
/// @param[out] out_min_time Optional destination earliest tick, initialized to zero.
/// @param[out] out_max_time Optional destination latest tick, initialized to zero.
/// @return Nonzero when at least one valid curve contributes a bound.
/// @details Builders sharing a parsed layer stack use that stack's active
/// transform-channel bounds. Legacy builders scan every direct TRS curve.
static int fbx_anim_compute_time_bounds(const fbx_anim_bone_builder_t *builders,
                                        int64_t bone_count,
                                        int64_t *out_min_time,
                                        int64_t *out_max_time) {
    int found = 0;
    int64_t min_time = 0;
    int64_t max_time = 0;
    if (out_min_time)
        *out_min_time = 0;
    if (out_max_time)
        *out_max_time = 0;
    if (builders)
        for (int64_t bone_idx = 0; bone_idx < bone_count; ++bone_idx)
            if (builders[bone_idx].layer_stack)
                return fbx_anim_layer_stack_bounds(
                    builders[bone_idx].layer_stack, 1, out_min_time, out_max_time);
    for (int64_t bone_idx = 0; bone_idx < bone_count; bone_idx++) {
        if (!builders[bone_idx].initialized)
            continue;
        for (int trs = 0; trs < 3; trs++) {
            for (int comp = 0; comp < 3; comp++) {
                const fbx_anim_curve_view_t *curve = &builders[bone_idx].curves[trs][comp];
                if (!fbx_anim_curve_has_data(curve))
                    continue;
                for (uint32_t k = 0; k < curve->count; k++) {
                    double seconds = (double)curve->times[k] / (double)FBX_TIME_SECOND;
                    if (!isfinite(seconds) || fabs(seconds) > FBX_ANIM_TIME_SECONDS_MAX)
                        continue;
                    if (!found || curve->times[k] < min_time)
                        min_time = curve->times[k];
                    if (!found || curve->times[k] > max_time)
                        max_time = curve->times[k];
                    found = 1;
                }
            }
        }
    }
    if (!found)
        return 0;
    if (out_min_time)
        *out_min_time = min_time;
    if (out_max_time)
        *out_max_time = max_time;
    return 1;
}

/// @brief Evaluate one bone builder's full transform-component record at an FBX tick.
/// @details Only authored local T/R/S lanes are replaced by animation curves. Rotation order,
///          activation, pivots, offsets, pre/post rotation, geometric transforms, and inheritance
///          remain copied from the static base record.
/// @param builder Initialized bone builder.
/// @param fbx_time Source time in FBX ticks.
/// @param out Receives the sampled and sanitized component record.
/// @return Nonzero on success.
static int fbx_anim_sample_components(const fbx_anim_bone_builder_t *builder,
                                      int64_t fbx_time,
                                      fbx_transform_components_t *out) {
    if (!builder || !builder->initialized || !out)
        return 0;
    if (builder->layer_stack)
        return fbx_anim_layer_sample_components(
            builder->layer_stack, builder->target_id, &builder->base_transform, fbx_time, out);
    *out = builder->base_transform;
    for (int component = 0; component < 3; component++) {
        out->translation[component] = fbx_anim_curve_value(
            &builder->curves[0][component], fbx_time, out->translation[component]);
        out->rotation[component] = fbx_anim_curve_value(
            &builder->curves[1][component], fbx_time, out->rotation[component]);
        out->scale[component] =
            fbx_anim_curve_value(&builder->curves[2][component], fbx_time, out->scale[component]);
        out->translation[component] =
            fbx_clamp_abs_or(out->translation[component], 0.0, FBX_NUMERIC_ABS_MAX);
        out->rotation[component] = fbx_sanitize_rotation_degrees(out->rotation[component]);
        out->scale[component] = fbx_scale_or_unit(out->scale[component]);
    }
    return 1;
}

/// @brief Read and validate a direct model curve for constraint-aware sampling.
/// @param[in] curve Candidate AnimationCurve node.
/// @param[out] out Destination borrowed curve view.
/// @return Nonzero when time/value arrays match and pass numeric/order
/// validation; otherwise zero.
static int fbx_anim_constraint_read_curve(fbx_node_t *curve, fbx_anim_curve_view_t *out) {
    uint32_t time_count = 0;
    uint32_t value_count = 0;
    uint32_t flag_count = 0;
    uint32_t data_count = 0;
    fbx_anim_curve_view_t candidate;
    if (!curve || !out || strcmp(curve->name, "AnimationCurve") != 0)
        return 0;
    memset(&candidate, 0, sizeof(candidate));
    candidate.times = fbx_get_i64_array(curve, "KeyTime", &time_count);
    candidate.values64 = fbx_get_f64_array(curve, "KeyValueFloat", &value_count);
    if (!candidate.values64)
        candidate.values32 = fbx_get_f32_array(curve, "KeyValueFloat", &value_count);
    candidate.attr_flags = fbx_get_i32_array(curve, "KeyAttrFlags", &flag_count);
    candidate.attr_data64 = fbx_get_f64_array(curve, "KeyAttrDataFloat", &data_count);
    if (!candidate.attr_data64)
        candidate.attr_data32 = fbx_get_f32_array(curve, "KeyAttrDataFloat", &data_count);
    candidate.count = time_count;
    candidate.attr_flag_count = candidate.attr_flags ? flag_count : 0;
    candidate.attr_data_count = (candidate.attr_data64 || candidate.attr_data32) ? data_count : 0;
    if (time_count == 0 || time_count != value_count || !fbx_anim_curve_view_valid(&candidate))
        return 0;
    *out = candidate;
    return 1;
}

/// @brief Build indexed animation records for every FBX Model object.
/// @param[in] objects FBX Objects node.
/// @param[in] ct Indexed connection graph used to resolve parent IDs.
/// @param[out] out_models Destination owned model-record array.
/// @param[out] out_count Destination model count.
/// @param[out] out_ids Destination owned ID-to-model-index table, sorted by ID.
/// @param[in,out] context Aggregate allocation budget.
/// @return Nonzero on success, including an empty model set; zero for invalid
/// arguments, budget exhaustion, overflow, or allocation failure.
/// @details Each record caches the source node, parent relationship, skeleton
/// classification, and initialized static transform components.
static int fbx_anim_constraint_make_models(fbx_node_t *objects,
                                           const fbx_conn_table_t *ct,
                                           fbx_anim_constraint_model_t **out_models,
                                           int32_t *out_count,
                                           fbx_model_id_entry_t **out_ids,
                                           fbx_load_context_t *context) {
    fbx_anim_constraint_model_t *models;
    fbx_model_id_entry_t *ids;
    int32_t count = 0;
    if (!objects || !ct || !out_models || !out_count || !out_ids || !context)
        return 0;
    *out_models = NULL;
    *out_count = 0;
    *out_ids = NULL;
    for (int32_t i = 0; i < objects->child_count; ++i)
        if (objects->children[i].name && strcmp(objects->children[i].name, "Model") == 0 &&
            objects->children[i].prop_count >= 3)
            ++count;
    if (count == 0)
        return 1;
    if ((size_t)count > SIZE_MAX / sizeof(*models) || (size_t)count > SIZE_MAX / sizeof(*ids) ||
        !fbx_budget_charge(context, (uint64_t)count, sizeof(*models)) ||
        !fbx_budget_charge(context, (uint64_t)count, sizeof(*ids)))
        return 0;
    models = (fbx_anim_constraint_model_t *)calloc((size_t)count, sizeof(*models));
    ids = (fbx_model_id_entry_t *)malloc((size_t)count * sizeof(*ids));
    if (!models || !ids) {
        free(models);
        free(ids);
        return 0;
    }
    count = 0;
    for (int32_t i = 0; i < objects->child_count; ++i) {
        fbx_node_t *model = &objects->children[i];
        if (!model->name || strcmp(model->name, "Model") != 0 || model->prop_count < 3)
            continue;
        models[count].id = fbx_prop_i64(model, 0);
        models[count].parent_id = fbx_find_parent(ct, models[count].id);
        models[count].parent_index = -1;
        models[count].import_index = i;
        models[count].is_skeleton = fbx_model_type_is_skeleton(fbx_prop_str(model, 2));
        models[count].source = model;
        models[count].animation.target_id = models[count].id;
        fbx_read_model_transform_components(model, &models[count].animation.base_transform);
        models[count].animation.initialized = 1;
        ids[count].id = models[count].id;
        ids[count].index = count;
        ++count;
    }
    qsort(ids, (size_t)count, sizeof(*ids), fbx_model_id_entry_cmp);
    for (int32_t i = 0; i < count; ++i)
        if (models[i].parent_id != 0)
            models[i].parent_index = fbx_model_index_for_id(ids, count, models[i].parent_id);
    *out_models = models;
    *out_count = count;
    *out_ids = ids;
    return 1;
}

/// @brief Attach stack animation sources to every all-model constraint record.
/// @param[in] objects FBX Objects node.
/// @param[in] ct Indexed connection graph.
/// @param[in] stack_id Numeric AnimationStack identifier.
/// @param[in] layer_stack Parsed shared layer stack, or `NULL` for legacy
/// direct-curve collection.
/// @param[in,out] models All-model animation records.
/// @param[in] model_count Number of model records and ID mappings.
/// @param[in] ids Sorted ID-to-model-index table.
/// @details A parsed stack is shared directly by every model sampler. The
/// legacy path walks layer curve nodes and attaches the first valid curve for
/// each model/TRS/component slot.
static void fbx_anim_constraint_collect_curves(fbx_node_t *objects,
                                               const fbx_conn_table_t *ct,
                                               int64_t stack_id,
                                               const fbx_anim_layer_stack_t *layer_stack,
                                               fbx_anim_constraint_model_t *models,
                                               int32_t model_count,
                                               const fbx_model_id_entry_t *ids) {
    if (layer_stack) {
        for (int32_t model = 0; model < model_count; ++model)
            models[model].animation.layer_stack = layer_stack;
        return;
    }
    for (int32_t layer_connection = fbx_connections_first_for_parent(ct, stack_id);
         layer_connection >= 0;
         layer_connection = fbx_connections_next_for_parent(ct, layer_connection)) {
        fbx_node_t *layer = fbx_find_object_by_id(objects, ct->entries[layer_connection].child_id);
        if (!layer || strcmp(layer->name, "AnimationLayer") != 0 || layer->prop_count < 1)
            continue;
        for (int32_t node_connection = fbx_connections_first_for_parent(ct, fbx_prop_i64(layer, 0));
             node_connection >= 0;
             node_connection = fbx_connections_next_for_parent(ct, node_connection)) {
            int64_t curve_node_id = ct->entries[node_connection].child_id;
            fbx_node_t *curve_node = fbx_find_object_by_id(objects, curve_node_id);
            int32_t model_index = -1;
            int32_t trs = -1;
            if (!curve_node || strcmp(curve_node->name, "AnimationCurveNode") != 0)
                continue;
            for (int32_t target_connection = fbx_connections_first_for_child(ct, curve_node_id);
                 target_connection >= 0;
                 target_connection = fbx_connections_next_for_child(ct, target_connection)) {
                fbx_node_t *target =
                    fbx_find_object_by_id(objects, ct->entries[target_connection].parent_id);
                const char *property = ct->entries[target_connection].prop;
                if (!target || strcmp(target->name, "Model") != 0 || !property)
                    continue;
                if (strcmp(property, "Lcl Translation") == 0)
                    trs = 0;
                else if (strcmp(property, "Lcl Rotation") == 0)
                    trs = 1;
                else if (strcmp(property, "Lcl Scaling") == 0)
                    trs = 2;
                if (trs >= 0) {
                    model_index = fbx_model_index_for_id(
                        ids, model_count, ct->entries[target_connection].parent_id);
                    break;
                }
            }
            if (model_index < 0 || trs < 0)
                continue;
            for (int32_t curve_connection = fbx_connections_first_for_parent(ct, curve_node_id);
                 curve_connection >= 0;
                 curve_connection = fbx_connections_next_for_parent(ct, curve_connection)) {
                int component =
                    fbx_anim_component_from_connection_prop(ct->entries[curve_connection].prop);
                fbx_node_t *curve;
                if (component < 0 ||
                    fbx_anim_curve_has_data(&models[model_index].animation.curves[trs][component]))
                    continue;
                curve = fbx_find_object_by_id(objects, ct->entries[curve_connection].child_id);
                (void)fbx_anim_constraint_read_curve(
                    curve, &models[model_index].animation.curves[trs][component]);
            }
        }
    }
}

/// @brief Compute the tick extent of all model curves used by constraint baking.
/// @param[in] models All-model animation records.
/// @param[in] model_count Number of records.
/// @param[out] minimum Destination earliest tick.
/// @param[out] maximum Destination latest tick.
/// @return Nonzero when at least one direct curve or active parsed-stack
/// transform curve contributes bounds.
static int fbx_anim_constraint_bounds(const fbx_anim_constraint_model_t *models,
                                      int32_t model_count,
                                      int64_t *minimum,
                                      int64_t *maximum) {
    int found = 0;
    for (int32_t model = 0; model < model_count; ++model)
        if (models[model].animation.layer_stack)
            return fbx_anim_layer_stack_bounds(
                models[model].animation.layer_stack, 1, minimum, maximum);
    for (int32_t model = 0; model < model_count; ++model)
        for (int trs = 0; trs < 3; ++trs)
            for (int component = 0; component < 3; ++component) {
                const fbx_anim_curve_view_t *curve =
                    &models[model].animation.curves[trs][component];
                if (!fbx_anim_curve_has_data(curve))
                    continue;
                if (!found || curve->times[0] < *minimum)
                    *minimum = curve->times[0];
                if (!found || curve->times[curve->count - 1] > *maximum)
                    *maximum = curve->times[curve->count - 1];
                found = 1;
            }
    return found;
}

/// @brief Allocate and populate a reusable constraint-evaluation pose.
/// @param[in] models All-model records supplying IDs, parents, and source nodes.
/// @param[in] model_count Number of pose entries.
/// @param[in,out] context Aggregate allocation budget.
/// @param[out] pose Destination pose.
/// @return Nonzero when pose storage is allocated and its sorted lookup table
/// initialized; otherwise zero.
static int fbx_anim_constraint_initialize_pose(const fbx_anim_constraint_model_t *models,
                                               int32_t model_count,
                                               fbx_load_context_t *context,
                                               fbx_constraint_pose_t *pose) {
    if (!fbx_constraint_pose_allocate(pose, model_count, context))
        return 0;
    for (int32_t i = 0; i < model_count; ++i) {
        pose->ids[i] = models[i].id;
        pose->parent_indices[i] = models[i].parent_index;
        pose->sources[i] = models[i].source;
        pose->id_table[i].id = models[i].id;
        pose->id_table[i].index = i;
    }
    qsort(pose->id_table, (size_t)model_count, sizeof(*pose->id_table), fbx_model_id_entry_cmp);
    return 1;
}

/// @brief Determine whether applying a constraint can change one model.
/// @param[in] constraint Constraint to inspect.
/// @param[in] pose Pose providing IDs and IK parent relationships.
/// @param[in] model_index Candidate model index.
/// @return Nonzero when an active supported constraint targets the model.
/// @details Ordinary constraints write their direct target. A single-chain IK
/// constraint writes every model on the end-effector-to-target chain.
static int fbx_anim_constraint_writes_model(const fbx_constraint_t *constraint,
                                            const fbx_constraint_pose_t *pose,
                                            int32_t model_index) {
    if (!constraint || !constraint->active || constraint->kind == FBX_CONSTRAINT_UNKNOWN || !pose ||
        model_index < 0 || model_index >= pose->count)
        return 0;
    if (constraint->kind != FBX_CONSTRAINT_SINGLE_CHAIN_IK)
        return constraint->target_id == pose->ids[model_index];
    for (int32_t current = fbx_constraint_pose_index(pose, constraint->ik_end_id), guard = 0;
         current >= 0 && guard++ <= pose->count;
         current = pose->parent_indices[current]) {
        if (current == model_index)
            return 1;
        if (pose->ids[current] == constraint->target_id)
            break;
    }
    return 0;
}

/// @brief Collect every source time capable of affecting one constrained model.
/// @param[in,out] times Address of the unsorted dynamic tick array.
/// @param[in,out] count Address of its logical count.
/// @param[in,out] capacity Address of its allocated capacity.
/// @param[in] models All-model animation records.
/// @param[in] model_count Number of model and pose entries.
/// @param[in] model_index Output model whose dependency closure is collected.
/// @param[in] constraints Parsed constraint set.
/// @param[in,out] pose Reusable pose whose evaluation flags serve as marks.
/// @param[in,out] context Aggregate allocation budget.
/// @return Nonzero when the dependency closure and all of its direct or layered
/// curve times are collected; otherwise zero.
/// @details The closure includes ancestors plus source, world-up, IK effector,
/// and pole models of every constraint that can write an already marked model.
static int fbx_anim_constraint_append_times(int64_t **times,
                                            int32_t *count,
                                            int32_t *capacity,
                                            const fbx_anim_constraint_model_t *models,
                                            int32_t model_count,
                                            int32_t model_index,
                                            const fbx_constraint_set_t *constraints,
                                            fbx_constraint_pose_t *pose,
                                            fbx_load_context_t *context) {
    uint8_t *marked;
    if (!models || model_count <= 0 || model_index < 0 || model_index >= model_count || !pose ||
        pose->count != model_count || !pose->evaluation_flags)
        return 0;
    marked = pose->evaluation_flags;
    memset(marked, 0, (size_t)model_count * sizeof(*marked));
    marked[model_index] = 1;
    for (int32_t pass = 0; pass < model_count + constraints->count; ++pass) {
        int progress = 0;
        for (int32_t current = 0; current < model_count; ++current) {
            int32_t parent;
            if (!marked[current])
                continue;
            parent = models[current].parent_index;
            if (parent >= 0 && !marked[parent]) {
                marked[parent] = 1;
                progress = 1;
            }
            for (int32_t ci = 0; ci < constraints->count; ++ci) {
                const fbx_constraint_t *constraint = &constraints->entries[ci];
                if (!fbx_anim_constraint_writes_model(constraint, pose, current))
                    continue;
                for (int32_t source = 0; source < constraint->source_count; ++source) {
                    int32_t dependency =
                        fbx_constraint_pose_index(pose, constraint->sources[source].model_id);
                    if (dependency >= 0 && !marked[dependency]) {
                        marked[dependency] = 1;
                        progress = 1;
                    }
                }
                const int64_t roles[] = {
                    constraint->world_up_id, constraint->ik_effector_id, constraint->ik_pole_id};
                for (size_t role = 0; role < sizeof(roles) / sizeof(roles[0]); ++role) {
                    int32_t dependency = fbx_constraint_pose_index(pose, roles[role]);
                    if (dependency >= 0 && !marked[dependency]) {
                        marked[dependency] = 1;
                        progress = 1;
                    }
                }
            }
        }
        if (!progress)
            break;
    }
    for (int32_t model = 0; model < model_count; ++model) {
        if (!marked[model])
            continue;
        if (models[model].animation.layer_stack) {
            if (!fbx_anim_layer_stack_append_times(times,
                                                   count,
                                                   capacity,
                                                   models[model].animation.layer_stack,
                                                   models[model].id,
                                                   1,
                                                   NULL,
                                                   context))
                return 0;
            continue;
        }
        for (int trs = 0; trs < 3; ++trs)
            for (int component = 0; component < 3; ++component)
                if (!fbx_anim_append_curve_sample_times(
                        times,
                        count,
                        capacity,
                        &models[model].animation.curves[trs][component],
                        context))
                    return 0;
    }
    return 1;
}

/// @brief Map a runtime bone index into the all-model constraint pose.
/// @param[in] bone_bindings Numeric model-to-bone mappings.
/// @param[in] bone_binding_count Number of mappings.
/// @param[in] pose Constraint pose providing model-ID lookup.
/// @param[in] bone_index Runtime skeleton bone index.
/// @return The corresponding pose model index, or `-1`.
static int32_t fbx_anim_constraint_model_for_bone(const fbx_bone_binding_t *bone_bindings,
                                                  int32_t bone_binding_count,
                                                  const fbx_constraint_pose_t *pose,
                                                  int32_t bone_index) {
    for (int32_t i = 0; i < bone_binding_count; ++i)
        if (bone_bindings[i].bone_index == bone_index)
            return fbx_constraint_pose_index(pose, bone_bindings[i].model_id);
    return -1;
}

/// @brief Evaluate one model's constrained parent-relative runtime transform.
/// @param[in] models All-model animation records.
/// @param[in] model_count Number of model and pose entries.
/// @param[in] model_index Target model index.
/// @param[in] parent_model_index Runtime bone parent's model index, or `-1`.
/// @param[in] source_time Source time in FBX ticks.
/// @param[in] z_up Nonzero to apply Z-up to Y-up basis conversion.
/// @param[in,out] constraints Parsed set whose runtime caches may be updated.
/// @param[in,out] pose Reusable pose buffers.
/// @param[in,out] context Aggregate allocation and evaluation context.
/// @param[out] position Destination local translation.
/// @param[out] quaternion Destination local `(x, y, z, w)` rotation.
/// @param[out] scale Destination local scale.
/// @return Nonzero on success, or zero when sampling, hierarchy composition,
/// constraint application, or parent inversion fails.
/// @details All FBX models are sampled before the constraint set is applied;
/// the target global transform is then converted back relative to the runtime
/// skeleton parent and decomposed.
static int fbx_anim_constraint_sample_pose(const fbx_anim_constraint_model_t *models,
                                           int32_t model_count,
                                           int32_t model_index,
                                           int32_t parent_model_index,
                                           int64_t source_time,
                                           int z_up,
                                           fbx_constraint_set_t *constraints,
                                           fbx_constraint_pose_t *pose,
                                           fbx_load_context_t *context,
                                           double position[3],
                                           double quaternion[4],
                                           double scale[3]) {
    double runtime_local[16];
    pose->z_up = z_up;
    memset(pose->fixed_globals, 0, (size_t)model_count * sizeof(*pose->fixed_globals));
    for (int32_t model = 0; model < model_count; ++model) {
        if (!fbx_anim_sample_components(
                &models[model].animation, source_time, &pose->components[model]))
            return 0;
        fbx_build_model_transform_matrix(&pose->components[model],
                                         fbx_constraint_pose_local(pose, model));
        if (z_up)
            fbx_mat4_apply_zup(fbx_constraint_pose_local(pose, model));
    }
    if (!fbx_constraint_pose_recompute(pose) ||
        !fbx_constraint_apply_set(constraints, pose, context))
        return 0;
    if (parent_model_index >= 0) {
        double inverse_parent[16];
        if (!fbx_mat4_inverse_affine(fbx_constraint_pose_global(pose, parent_model_index),
                                     inverse_parent))
            return 0;
        fbx_mat4_mul_local(
            inverse_parent, fbx_constraint_pose_global(pose, model_index), runtime_local);
    } else {
        memcpy(runtime_local, fbx_constraint_pose_global(pose, model_index), sizeof(runtime_local));
    }
    fbx_decompose_matrix_trs(runtime_local, position, quaternion, scale);
    return 1;
}

/// @brief Bake one constrained skeleton bone into an Animation3D channel.
/// @param[in,out] animation Destination runtime clip.
/// @param[in] models All-model animation records.
/// @param[in] model_count Number of model and pose entries.
/// @param[in] model_index Model corresponding to the emitted bone.
/// @param[in] parent_model_index Runtime bone parent's model, or `-1`.
/// @param[in] bone_index Destination runtime bone index.
/// @param[in] clip_start Earliest source tick used as runtime time zero.
/// @param[in] z_up Nonzero to apply basis conversion.
/// @param[in,out] constraints Parsed constraint set.
/// @param[in,out] pose Reusable constraint pose.
/// @param[in,out] context Aggregate allocation budget.
/// @return One when keys were emitted, zero when no dependency curve supplied
/// times, or `-1` on collection, budget, pose, allocation, or insertion failure.
static int fbx_anim_constraint_emit_bone(void *animation,
                                         const fbx_anim_constraint_model_t *models,
                                         int32_t model_count,
                                         int32_t model_index,
                                         int32_t parent_model_index,
                                         int32_t bone_index,
                                         int64_t clip_start,
                                         int z_up,
                                         fbx_constraint_set_t *constraints,
                                         fbx_constraint_pose_t *pose,
                                         fbx_load_context_t *context) {
    int64_t *times = NULL;
    int32_t count = 0;
    int32_t capacity = 0;
    if (!fbx_anim_constraint_append_times(&times,
                                          &count,
                                          &capacity,
                                          models,
                                          model_count,
                                          model_index,
                                          constraints,
                                          pose,
                                          context)) {
        free(times);
        return -1;
    }
    fbx_anim_sort_unique_times(times, &count);
    if (count == 0) {
        free(times);
        return 0;
    }
    {
        int32_t reserved = 16;
        while (reserved < count) {
            if (reserved > RT_ANIMATION3D_MAX_KEYFRAMES_PER_CHANNEL / 2) {
                free(times);
                return -1;
            }
            reserved *= 2;
        }
        if (!fbx_budget_charge(context, (uint64_t)reserved, sizeof(vgfx3d_keyframe_t)) ||
            !fbx_budget_charge(context, 8u, sizeof(vgfx3d_anim_channel_t))) {
            free(times);
            return -1;
        }
    }
    for (int32_t i = 0; i < count; ++i) {
        double relative_time = ((double)times[i] - (double)clip_start) / (double)FBX_TIME_SECOND;
        double position_value[3];
        double quaternion_value[4];
        double scale_value[3];
        void *position;
        void *rotation;
        void *scale;
        if (!fbx_anim_constraint_sample_pose(models,
                                             model_count,
                                             model_index,
                                             parent_model_index,
                                             times[i],
                                             z_up,
                                             constraints,
                                             pose,
                                             context,
                                             position_value,
                                             quaternion_value,
                                             scale_value)) {
            free(times);
            return -1;
        }
        position = rt_vec3_new(position_value[0], position_value[1], position_value[2]);
        rotation = rt_quat_new(
            quaternion_value[0], quaternion_value[1], quaternion_value[2], quaternion_value[3]);
        scale = rt_vec3_new(scale_value[0], scale_value[1], scale_value[2]);
        if (!position || !rotation || !scale || !isfinite(relative_time) || relative_time < 0.0 ||
            !rt_animation3d_try_add_keyframe(
                animation, bone_index, relative_time, position, rotation, scale)) {
            fbx_release_ref(&position);
            fbx_release_ref(&rotation);
            fbx_release_ref(&scale);
            free(times);
            return -1;
        }
        fbx_release_ref(&position);
        fbx_release_ref(&rotation);
        fbx_release_ref(&scale);
    }
    free(times);
    return 1;
}

/// @brief Transactionally extract clips whose poses depend on FBX constraints.
/// @param[in] objects FBX Objects node.
/// @param[in] ct Indexed connection graph.
/// @param[in] skeleton Target runtime skeleton.
/// @param[in] bone_bindings Numeric model-to-bone mappings.
/// @param[in] bone_binding_count Number of mappings.
/// @param[in] z_up Nonzero for Z-up source content.
/// @param[in,out] constraints Parsed constraint set.
/// @param[in,out] out_anims Address of the owned output clip-reference array.
/// @param[in,out] out_count Address of its logical count.
/// @param[in,out] out_capacity Address of its allocated capacity.
/// @param[in,out] context Aggregate allocation budget.
/// @return Nonzero after processing every stack, including when no clip emits;
/// zero after releasing the entire staged output on a hard failure.
/// @details Each qualifying stack samples every model, applies constraints at
/// the dependency-union times for each skeleton bone, emits a looping clip, and
/// publishes it only when at least one bone produced keys.
static int fbx_extract_constraint_animations(fbx_node_t *objects,
                                             const fbx_conn_table_t *ct,
                                             void *skeleton,
                                             const fbx_bone_binding_t *bone_bindings,
                                             int32_t bone_binding_count,
                                             int z_up,
                                             fbx_constraint_set_t *constraints,
                                             void ***out_anims,
                                             int32_t *out_count,
                                             int32_t *out_capacity,
                                             fbx_load_context_t *context) {
    int64_t bone_count = rt_skeleton3d_get_bone_count(skeleton);
    for (int32_t stack_index = 0; stack_index < objects->child_count; ++stack_index) {
        fbx_node_t *stack = &objects->children[stack_index];
        fbx_anim_constraint_model_t *models = NULL;
        fbx_model_id_entry_t *ids = NULL;
        fbx_anim_layer_stack_t layer_stack;
        fbx_constraint_pose_t pose;
        int32_t model_count = 0;
        int64_t minimum = 0;
        int64_t maximum = 0;
        void *animation = NULL;
        int emitted = 0;
        memset(&pose, 0, sizeof(pose));
        memset(&layer_stack, 0, sizeof(layer_stack));
        if (strcmp(stack->name, "AnimationStack") != 0 || stack->prop_count < 1 ||
            !fbx_anim_stack_has_layer(objects, ct, fbx_prop_i64(stack, 0)))
            continue;
        if (!fbx_anim_layer_stack_parse(objects, ct, fbx_prop_i64(stack, 0), context, &layer_stack))
            goto fail;
        if (!fbx_anim_constraint_make_models(objects, ct, &models, &model_count, &ids, context)) {
            fbx_anim_layer_stack_free(&layer_stack);
            goto fail;
        }
        fbx_anim_constraint_collect_curves(
            objects, ct, fbx_prop_i64(stack, 0), &layer_stack, models, model_count, ids);
        if (!fbx_anim_constraint_bounds(models, model_count, &minimum, &maximum)) {
            free(models);
            free(ids);
            fbx_anim_layer_stack_free(&layer_stack);
            continue;
        }
        if (!fbx_anim_constraint_initialize_pose(models, model_count, context, &pose)) {
            free(models);
            free(ids);
            fbx_anim_layer_stack_free(&layer_stack);
            goto fail;
        }
        {
            rt_string name = fbx_object_name_string(stack, 1);
            double duration = ((double)maximum - (double)minimum) / (double)FBX_TIME_SECOND;
            if (!name || rt_str_is_empty(name)) {
                rt_string_unref(name);
                name = rt_const_cstr("Untitled");
            }
            animation = rt_animation3d_new(name, duration > 0.0 ? duration : 1.0);
            rt_string_unref(name);
        }
        if (!animation) {
            fbx_constraint_pose_free(&pose);
            free(models);
            free(ids);
            fbx_anim_layer_stack_free(&layer_stack);
            goto fail;
        }
        for (int32_t bone = 0; bone < bone_count; ++bone) {
            int32_t model_index =
                fbx_anim_constraint_model_for_bone(bone_bindings, bone_binding_count, &pose, bone);
            int64_t parent_bone = rt_skeleton3d_get_bone_parent_raw(skeleton, bone);
            int32_t parent_model =
                parent_bone >= 0
                    ? fbx_anim_constraint_model_for_bone(
                          bone_bindings, bone_binding_count, &pose, (int32_t)parent_bone)
                    : -1;
            int status;
            if (model_index < 0)
                continue;
            status = fbx_anim_constraint_emit_bone(animation,
                                                   models,
                                                   model_count,
                                                   model_index,
                                                   parent_model,
                                                   bone,
                                                   minimum,
                                                   z_up,
                                                   constraints,
                                                   &pose,
                                                   context);
            if (status < 0) {
                fbx_release_ref(&animation);
                fbx_constraint_pose_free(&pose);
                free(models);
                free(ids);
                fbx_anim_layer_stack_free(&layer_stack);
                goto fail;
            }
            if (status > 0)
                emitted = 1;
        }
        fbx_constraint_pose_free(&pose);
        free(models);
        free(ids);
        fbx_anim_layer_stack_free(&layer_stack);
        if (!emitted) {
            fbx_release_ref(&animation);
            continue;
        }
        rt_animation3d_set_looping(animation, 1);
        if (*out_count == INT32_MAX || !fbx_budget_charge(context, 1u, sizeof(void *)) ||
            !fbx_asset_reserve_ref_array(out_anims, out_capacity, *out_count + 1)) {
            fbx_release_ref(&animation);
            goto fail;
        }
        (*out_anims)[(*out_count)++] = animation;
    }
    return 1;
fail:
    fbx_asset_release_ref_array(out_anims, out_count, out_capacity);
    return 0;
}

/// @brief Evaluate a bone and all ancestors through the shared FBX inheritance composer.
/// @details The chain is sampled root-to-leaf at one source tick. Each global matrix uses
///          @ref fbx_build_inherited_global_matrix; the target is converted back to the
///          parent-relative matrix expected by Animation3D and decomposed with the same routine as
///          static SceneNode3D construction. This also bakes non-standard inheritance when parent
///          scale is animated.
/// @param builders Builder array indexed by runtime bone index.
/// @param bone_count Number of valid builders/bones.
/// @param skeleton Skeleton providing parent indices.
/// @param bone_idx Target bone index.
/// @param fbx_time Source time in ticks.
/// @param z_up Nonzero to apply basis conversion to every local matrix.
/// @param position Receives target parent-relative XYZ.
/// @param quaternion Receives target parent-relative XYZW.
/// @param scale Receives target parent-relative XYZ scale.
/// @return Nonzero on success, zero on an invalid/cyclic chain or singular parent transform.
static int fbx_anim_sample_bone_runtime_trs(const fbx_anim_bone_builder_t *builders,
                                            int64_t bone_count,
                                            void *skeleton,
                                            int64_t bone_idx,
                                            int64_t fbx_time,
                                            int z_up,
                                            double position[3],
                                            double quaternion[4],
                                            double scale[3]) {
    int32_t chain[FBX_MAX_SKELETON_BONES];
    int32_t chain_count = 0;
    int64_t walk = bone_idx;
    double parent_global[16];
    fbx_transform_components_t parent_components;
    int has_parent = 0;
    if (!builders || !skeleton || bone_count <= 0 || bone_count > FBX_MAX_SKELETON_BONES ||
        bone_idx < 0 || bone_idx >= bone_count) {
        return 0;
    }
    while (walk >= 0) {
        if (walk >= bone_count || chain_count >= FBX_MAX_SKELETON_BONES)
            return 0;
        for (int32_t i = 0; i < chain_count; i++)
            if (chain[i] == (int32_t)walk)
                return 0;
        chain[chain_count++] = (int32_t)walk;
        walk = rt_skeleton3d_get_bone_parent_raw(skeleton, walk);
    }
    for (int32_t chain_index = chain_count - 1; chain_index >= 0; chain_index--) {
        int32_t current = chain[chain_index];
        fbx_transform_components_t components;
        double local[16];
        double global[16];
        double runtime_local[16];
        if (!fbx_anim_sample_components(&builders[current], fbx_time, &components))
            return 0;
        fbx_build_model_transform_matrix(&components, local);
        if (z_up)
            fbx_mat4_apply_zup(local);
        if (has_parent) {
            double inverse_parent[16];
            fbx_build_inherited_global_matrix(
                parent_global, &parent_components, local, &components, global);
            if (!fbx_mat4_inverse_affine(parent_global, inverse_parent))
                return 0;
            fbx_mat4_mul_local(inverse_parent, global, runtime_local);
        } else {
            memcpy(global, local, sizeof(global));
            memcpy(runtime_local, local, sizeof(runtime_local));
        }
        if (chain_index == 0)
            fbx_decompose_matrix_trs(runtime_local, position, quaternion, scale);
        memcpy(parent_global, global, sizeof(parent_global));
        parent_components = components;
        has_parent = 1;
    }
    return 1;
}

/// @brief Append curve sample times for a bone and every ancestor affecting its inherited pose.
/// @details Parent curve times are included so non-standard scaling inheritance is baked whenever a
///          parent changes, even when the child has no directly authored curve at that instant.
/// @param times Dynamic unsorted tick array.
/// @param count Current element count.
/// @param capacity Current capacity.
/// @param builders Builder array indexed by bone.
/// @param bone_count Builder count.
/// @param skeleton Skeleton parent table.
/// @param bone_idx Starting bone.
/// @param context Active aggregate budget.
/// @return Nonzero on success, zero on invalid hierarchy or sample-limit/budget failure.
static int fbx_anim_append_inherited_sample_times(int64_t **times,
                                                  int32_t *count,
                                                  int32_t *capacity,
                                                  const fbx_anim_bone_builder_t *builders,
                                                  int64_t bone_count,
                                                  void *skeleton,
                                                  int64_t bone_idx,
                                                  fbx_load_context_t *context) {
    int32_t guard = 0;
    for (int64_t current = bone_idx; current >= 0;
         current = rt_skeleton3d_get_bone_parent_raw(skeleton, current)) {
        if (current >= bone_count || ++guard > bone_count)
            return 0;
        if (!builders[current].initialized)
            return 0;
        if (builders[current].layer_stack) {
            if (!fbx_anim_layer_stack_append_times(times,
                                                   count,
                                                   capacity,
                                                   builders[current].layer_stack,
                                                   builders[current].target_id,
                                                   1,
                                                   NULL,
                                                   context))
                return 0;
            continue;
        }
        for (int trs = 0; trs < 3; trs++) {
            for (int component = 0; component < 3; component++) {
                const fbx_anim_curve_view_t *curve = &builders[current].curves[trs][component];
                if (!fbx_anim_append_curve_sample_times(times, count, capacity, curve, context)) {
                    return 0;
                }
            }
        }
    }
    return 1;
}

/// @brief Emit one bone's inheritance-aware, fully composed Animation3D keyframes.
/// @details Returns -1 on any allocation, budget, hierarchy, or insertion failure so the caller can
///          roll back the complete FBX asset; zero means no source/ancestor curve affected this
///          bone, and one means at least one keyframe was emitted.
/// @param[in,out] anim Destination runtime animation.
/// @param[in] builders Per-bone animation samplers.
/// @param[in] bone_count Number of builder and skeleton entries.
/// @param[in] skeleton Target skeleton providing parent relationships.
/// @param[in] bone_idx Bone whose local channel is emitted.
/// @param[in] z_up Nonzero to apply source basis conversion.
/// @param[in] clip_start_time Source tick corresponding to runtime time zero.
/// @param[in,out] context Aggregate allocation budget.
/// @return One when keys were emitted, zero when no relevant times exist, or
/// `-1` on a hard failure.
static int fbx_anim_build_bone_keyframes(void *anim,
                                         const fbx_anim_bone_builder_t *builders,
                                         int64_t bone_count,
                                         void *skeleton,
                                         int64_t bone_idx,
                                         int z_up,
                                         int64_t clip_start_time,
                                         fbx_load_context_t *context) {
    int64_t *times = NULL;
    int32_t time_count = 0;
    int32_t time_capacity = 0;
    int emitted_any = 0;
    if (!builders[bone_idx].initialized)
        return 0;
    if (!fbx_anim_append_inherited_sample_times(&times,
                                                &time_count,
                                                &time_capacity,
                                                builders,
                                                bone_count,
                                                skeleton,
                                                bone_idx,
                                                context)) {
        rt_asset_error_set_if_empty(
            context && context->budget_exhausted ? RT_ASSET_ERROR_TOO_LARGE
                                                 : RT_ASSET_ERROR_CORRUPT,
            context && context->budget_exhausted
                ? "FBX.Load: animation-layer sample times exceed the aggregate load budget"
                : "FBX.Load: failed to collect animation-layer sample times");
        free(times);
        return -1;
    }
    fbx_anim_sort_unique_times(times, &time_count);
    if (time_count > 0) {
        int32_t reserved_keyframes = 16;
        while (reserved_keyframes < time_count) {
            if (reserved_keyframes > RT_ANIMATION3D_MAX_KEYFRAMES_PER_CHANNEL / 2) {
                rt_asset_error_set_if_empty(
                    RT_ASSET_ERROR_TOO_LARGE,
                    "FBX.Load: animation-layer channel exceeds the keyframe limit");
                free(times);
                return -1;
            }
            reserved_keyframes *= 2;
        }
        if (!fbx_budget_charge(context, (uint64_t)reserved_keyframes, sizeof(vgfx3d_keyframe_t)) ||
            !fbx_budget_charge(context, 8u, sizeof(vgfx3d_anim_channel_t))) {
            rt_asset_error_set_if_empty(
                RT_ASSET_ERROR_TOO_LARGE,
                "FBX.Load: animation-layer keyframes exceed the aggregate load budget");
            free(times);
            return -1;
        }
    }
    for (int32_t ti = 0; ti < time_count; ti++) {
        int64_t fbx_time = times[ti];
        double t = ((double)fbx_time - (double)clip_start_time) / (double)FBX_TIME_SECOND;
        double position_value[3];
        double quaternion_value[4];
        double scale_value[3];
        void *position;
        void *rotation;
        void *scale;
        if (isfinite(t) && t < 0.0 && t > -1e-9)
            t = 0.0;
        if (!isfinite(t) || t < 0.0 || t > FBX_ANIM_TIME_SECONDS_MAX)
            continue;
        if (!fbx_anim_sample_bone_runtime_trs(builders,
                                              bone_count,
                                              skeleton,
                                              bone_idx,
                                              fbx_time,
                                              z_up,
                                              position_value,
                                              quaternion_value,
                                              scale_value)) {
            rt_asset_error_set_if_empty(
                RT_ASSET_ERROR_CORRUPT,
                "FBX.Load: animation layer transform evaluation produced an invalid pose");
            free(times);
            return -1;
        }
        position = rt_vec3_new(position_value[0], position_value[1], position_value[2]);
        rotation = rt_quat_new(
            quaternion_value[0], quaternion_value[1], quaternion_value[2], quaternion_value[3]);
        scale = rt_vec3_new(scale_value[0], scale_value[1], scale_value[2]);
        if (!position || !rotation || !scale ||
            !rt_animation3d_try_add_keyframe(anim, bone_idx, t, position, rotation, scale)) {
            rt_asset_error_set_if_empty(
                RT_ASSET_ERROR_CORRUPT,
                "FBX.Load: failed to store a composed animation-layer bone keyframe");
            fbx_release_ref(&position);
            fbx_release_ref(&rotation);
            fbx_release_ref(&scale);
            free(times);
            return -1;
        }
        emitted_any = 1;
        fbx_release_ref(&position);
        fbx_release_ref(&rotation);
        fbx_release_ref(&scale);
    }
    free(times);
    return emitted_any;
}

/// @brief Extract animation clips transactionally from the indexed FBX node graph.
/// @details Each stack samples full FBX component/inheritance matrices over authored and adaptive
///          times. Builder and generated runtime storage are charged before allocation. Any hard
///          failure releases all clips staged by this call so the enclosing asset cannot publish a
///          truncated animation set.
/// @param[in] root FBX document root.
/// @param[in] ct Indexed connection graph.
/// @param[in] skeleton Target runtime skeleton.
/// @param[in] bone_bindings Numeric model-to-bone mappings.
/// @param[in] bone_binding_count Number of mappings.
/// @param[in] z_up Nonzero for Z-up source content.
/// @param[in,out] constraints Optional parsed constraints; a nonempty set
/// selects constraint-aware baking for every stack.
/// @param[out] out_anims Receives a newly allocated array of retained animation
/// objects owned by the caller.
/// @param[out] out_count Receives the number of animations written.
/// @param[out] out_capacity Receives the allocated reference-array capacity.
/// @param[in,out] context Active aggregate load budget.
/// @return Nonzero on success (including no animations), zero after rolling back on failure.
static int fbx_extract_animations(fbx_node_t *root,
                                  const fbx_conn_table_t *ct,
                                  void *skeleton,
                                  const fbx_bone_binding_t *bone_bindings,
                                  int32_t bone_binding_count,
                                  int z_up,
                                  fbx_constraint_set_t *constraints,
                                  void ***out_anims,
                                  int32_t *out_count,
                                  int32_t *out_capacity,
                                  fbx_load_context_t *context) {
    if (!out_anims || !out_count || !out_capacity)
        return 0;
    *out_anims = NULL;
    *out_count = 0;
    *out_capacity = 0;

    fbx_node_t *objects = fbx_find_child(root, "Objects");
    if (!objects)
        return 1;

    int64_t bone_count = skeleton ? rt_skeleton3d_get_bone_count(skeleton) : 0;
    if (bone_count <= 0)
        return 1;
    if (bone_count > FBX_MAX_SKELETON_BONES ||
        (size_t)bone_count > SIZE_MAX / sizeof(fbx_anim_bone_builder_t))
        return 0;
    if (constraints && constraints->count > 0)
        return fbx_extract_constraint_animations(objects,
                                                 ct,
                                                 skeleton,
                                                 bone_bindings,
                                                 bone_binding_count,
                                                 z_up,
                                                 constraints,
                                                 out_anims,
                                                 out_count,
                                                 out_capacity,
                                                 context);

    /* Find AnimationStack nodes */
    for (int32_t i = 0; i < objects->child_count; i++) {
        fbx_node_t *obj = &objects->children[i];
        fbx_anim_bone_builder_t *builders;
        fbx_anim_layer_stack_t layer_stack;
        int64_t min_time;
        int64_t max_time;
        double duration;
        if (strcmp(obj->name, "AnimationStack") != 0)
            continue;

        memset(&layer_stack, 0, sizeof(layer_stack));

        int64_t stack_id = fbx_prop_i64(obj, 0);
        rt_string runtime_anim_name = NULL;

        if (!fbx_anim_stack_has_layer(objects, ct, stack_id))
            continue;
        if (!fbx_anim_layer_stack_parse(objects, ct, stack_id, context, &layer_stack))
            goto fail;

        if ((size_t)bone_count > SIZE_MAX / sizeof(*builders) ||
            !fbx_budget_charge(context, (uint64_t)bone_count, sizeof(*builders))) {
            fbx_anim_layer_stack_free(&layer_stack);
            goto fail;
        }
        builders = (fbx_anim_bone_builder_t *)calloc((size_t)bone_count, sizeof(*builders));
        if (!builders) {
            fbx_anim_layer_stack_free(&layer_stack);
            goto fail;
        }

        fbx_anim_collect_curves(objects,
                                ct,
                                skeleton,
                                bone_bindings,
                                bone_binding_count,
                                bone_count,
                                stack_id,
                                &layer_stack,
                                builders);
        if (!fbx_anim_compute_time_bounds(builders, bone_count, &min_time, &max_time)) {
            free(builders);
            fbx_anim_layer_stack_free(&layer_stack);
            continue;
        }
        duration = ((double)max_time - (double)min_time) / (double)FBX_TIME_SECOND;
        duration = fbx_clamp_double(duration, 0.0, FBX_ANIM_TIME_SECONDS_MAX, 0.0);
        runtime_anim_name = fbx_object_name_string(obj, 1);
        if (!runtime_anim_name || rt_str_is_empty(runtime_anim_name)) {
            rt_string_unref(runtime_anim_name);
            runtime_anim_name = rt_const_cstr("Untitled");
        }
        void *anim = rt_animation3d_new(runtime_anim_name, duration > 0.0 ? duration : 1.0);
        rt_string_unref(runtime_anim_name);
        int emitted_any = 0;
        if (!anim) {
            rt_asset_error_set_if_empty(
                RT_ASSET_ERROR_CORRUPT,
                "FBX.Load: failed to allocate a composed animation-layer clip");
            free(builders);
            fbx_anim_layer_stack_free(&layer_stack);
            goto fail;
        }

        for (int64_t bone_idx = 0; bone_idx < bone_count; bone_idx++) {
            int build_status = fbx_anim_build_bone_keyframes(
                anim, builders, bone_count, skeleton, bone_idx, z_up, min_time, context);
            if (build_status < 0) {
                free(builders);
                fbx_anim_layer_stack_free(&layer_stack);
                fbx_release_ref(&anim);
                goto fail;
            }
            if (build_status > 0)
                emitted_any = 1;
        }
        free(builders);
        fbx_anim_layer_stack_free(&layer_stack);

        if (!emitted_any) {
            fbx_release_ref(&anim);
            continue;
        }
        rt_animation3d_set_looping(anim, 1);

        if (*out_count < 0 || *out_count == INT32_MAX) {
            rt_asset_error_set_if_empty(
                RT_ASSET_ERROR_TOO_LARGE,
                "FBX.Load: animation-layer clip count exceeds the runtime limit");
            fbx_release_ref(&anim);
            goto fail;
        }
        if (!fbx_budget_charge(context, 1u, sizeof(void *))) {
            rt_asset_error_set_if_empty(
                RT_ASSET_ERROR_TOO_LARGE,
                "FBX.Load: animation-layer clips exceed the aggregate load budget");
            fbx_release_ref(&anim);
            goto fail;
        }
        if (!fbx_asset_reserve_ref_array(out_anims, out_capacity, *out_count + 1)) {
            rt_asset_error_set_if_empty(
                RT_ASSET_ERROR_CORRUPT,
                "FBX.Load: failed to store a composed animation-layer clip");
            fbx_release_ref(&anim);
            goto fail;
        }
        (*out_anims)[*out_count] = anim;
        (*out_count)++;
    }
    return 1;
fail:
    fbx_asset_release_ref_array(out_anims, out_count, out_capacity);
    return 0;
}
