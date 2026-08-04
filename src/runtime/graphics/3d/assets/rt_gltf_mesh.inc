//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/assets/rt_gltf_mesh.inc
// Purpose: Implement glTF mesh import for preloaded, accessor-backed, and
//   compressed primitive paths. Included by rt_gltf.c.
// Key invariants:
//   - Parallel asset mesh/material/variant tables publish growth atomically.
//   - Imported mesh counts and capacities always describe initialized slots.
// Ownership/Lifetime:
//   - Imported meshes and side tables are owned by the destination glTF asset.
//   - Temporary decode storage remains borrowed or locally owned until commit.
// Links: rt_gltf.c, rt_gltf_internal.h
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_gltf_mesh.inc
 * @brief Implements glTF primitive decoding, skin-influence handling, and mesh table assembly.
 * @details Included by `rt_gltf.c`, this fragment imports worker-predecoded POD primitives,
 *          ordinary accessor-backed primitives, oversized-skeleton partitions, and Draco
 *          payloads. It validates material UV requirements, sanitizes vertex attributes, emits
 *          triangles for supported primitive modes, preserves morph and material-variant
 *          mappings, and grows asset tables when one source primitive expands into multiple
 *          runtime meshes.
 */

/// @brief Append one imported vertex to @p mesh during glTF import, taking position, normal,
///   UV0/UV1, color, and tangent (any pointer may be NULL to use that attribute's default).
/// @param mesh Mesh with preallocated vertex storage.
/// @param pos Three-component position.
/// @param nrm Three-component normal.
/// @param uv0 Primary two-component texture coordinates.
/// @param uv1 Secondary two-component texture coordinates.
/// @param color Four-component vertex color.
/// @param tangent Four-component tangent and handedness.
/// @return 1 on success, 0 if the mesh vertex storage cannot grow.
static int gltf_mesh_append_import_vertex(rt_mesh3d *mesh,
                                          const float *pos,
                                          const float *nrm,
                                          const float *uv0,
                                          const float *uv1,
                                          const float *color,
                                          const float *tangent) {
    vgfx3d_vertex_t *vertex;
    if (!mesh || mesh->build_failed || !pos || !nrm || !uv0 || !uv1 || !color || !tangent)
        return 0;
    if (mesh->vertex_count >= mesh->vertex_capacity || mesh->vertex_count == UINT32_MAX)
        return 0;
    vertex = &mesh->vertices[mesh->vertex_count++];
    memset(vertex, 0, sizeof(*vertex));
    vertex->pos[0] = pos[0];
    vertex->pos[1] = pos[1];
    vertex->pos[2] = pos[2];
    if (mesh->positions64) {
        mesh->positions64[(size_t)(mesh->vertex_count - 1u) * 3u + 0] = (double)pos[0];
        mesh->positions64[(size_t)(mesh->vertex_count - 1u) * 3u + 1] = (double)pos[1];
        mesh->positions64[(size_t)(mesh->vertex_count - 1u) * 3u + 2] = (double)pos[2];
    }
    vertex->normal[0] = nrm[0];
    vertex->normal[1] = nrm[1];
    vertex->normal[2] = nrm[2];
    vertex->uv[0] = uv0[0];
    vertex->uv[1] = uv0[1];
    vertex->uv1[0] = uv1[0];
    vertex->uv1[1] = uv1[1];
    memcpy(vertex->color, color, sizeof(vertex->color));
    memcpy(vertex->tangent, tangent, sizeof(vertex->tangent));
    return 1;
}

/// @brief Resolved and validated accessor views for one non-Draco primitive.
typedef struct {
    /// Required vertex-position accessor.
    gltf_accessor_view_t pos;
    /// Optional normal accessor.
    gltf_accessor_view_t norm;
    /// Optional primary texture-coordinate accessor.
    gltf_accessor_view_t uv0;
    /// Optional secondary texture-coordinate accessor.
    gltf_accessor_view_t uv1;
    /// Optional vertex-color accessor.
    gltf_accessor_view_t color;
    /// Optional tangent accessor.
    gltf_accessor_view_t tangent;
    /// Optional first joint-index set.
    gltf_accessor_view_t joints;
    /// Optional first joint-weight set.
    gltf_accessor_view_t weights;
    /// Optional second joint-index set.
    gltf_accessor_view_t joints1;
    /// Optional second joint-weight set.
    gltf_accessor_view_t weights1;
    /// Optional primitive index accessor.
    gltf_accessor_view_t idx;
    /// Whether @ref norm is present and usable.
    int has_normals;
    /// Whether @ref uv0 is present and usable.
    int has_uv0;
    /// Whether @ref uv1 is present and usable.
    int has_uv1;
    /// Whether @ref color is present and usable.
    int has_colors;
    /// Whether @ref tangent is present and usable.
    int has_tangents;
    /// Whether the first joint-index accessor resolved.
    int has_joints;
    /// Whether the first joint-weight accessor resolved.
    int has_weights;
    /// Whether the second joint-index accessor resolved.
    int has_joints1;
    /// Whether the second joint-weight accessor resolved.
    int has_weights1;
    /// Whether @ref idx is present and usable.
    int has_indices;
    /// Whether the first joint/weight pair forms a valid skin stream.
    int has_skin0;
    /// Whether the second joint/weight pair forms a valid skin stream.
    int has_skin1;
    /// Conservative number of triangles expected from the source draw mode.
    int32_t triangle_estimate;
} gltf_mesh_primitive_views_t;

/// @brief Resolve a primitive's material or lazily install the shared default PBR material.
/// @param asset Asset owning the material table.
/// @param material_idx Requested glTF material index.
/// @param material_capacity Allocated number of material slots.
/// @param default_material_io In/out cached default material handle.
/// @return Borrowed resolved material, or NULL when no valid material can be constructed.
static void *gltf_mesh_resolve_primitive_material(rt_gltf_asset *asset,
                                                  int64_t material_idx,
                                                  int32_t material_capacity,
                                                  void **default_material_io) {
    void *prim_material = NULL;
    if (!asset)
        return NULL;
    if (material_idx >= 0 && material_idx < asset->material_count)
        prim_material = asset->materials[material_idx];
    if (!prim_material && asset->materials) {
        if (default_material_io && !*default_material_io &&
            asset->material_count < material_capacity) {
            *default_material_io = gltf_material_create_default_pbr();
            if (*default_material_io)
                asset->materials[asset->material_count++] = *default_material_io;
        }
        prim_material = default_material_io ? *default_material_io : NULL;
    }
    return prim_material;
}

/**
 * @brief Validate one imported material's authored texture slots against primitive UV streams.
 * @details Only texture-info objects actually present in the source document impose a stream
 *          requirement. UV sets other than 0 and 1 are unsupported. A representable set whose
 *          accessor did not decode successfully is corrupt input. The first failure records a
 *          stable diagnostic naming the mesh, primitive, material, slot, and UV set.
 * @param asset Borrowed asset whose material count bounds @p material_index.
 * @param material_infos Borrowed per-material texture metadata table.
 * @param material_index glTF material index to validate; negative/default indices have no authored
 *        texture requirement.
 * @param has_uv0 Nonzero when TEXCOORD_0 decoded successfully for this primitive.
 * @param has_uv1 Nonzero when TEXCOORD_1 decoded successfully for this primitive.
 * @param mesh_json_index Source mesh index used in diagnostics.
 * @param primitive_index Source primitive index used in diagnostics.
 * @return 1 when every authored slot is satisfiable, otherwise 0 with an asset error recorded.
 */
static int gltf_mesh_validate_material_uv_streams(const rt_gltf_asset *asset,
                                                  const gltf_material_info_t *material_infos,
                                                  int64_t material_index,
                                                  int has_uv0,
                                                  int has_uv1,
                                                  int mesh_json_index,
                                                  int primitive_index) {
    static const char *const slot_names[RT_MATERIAL3D_TEXTURE_SLOT_COUNT] = {
        "baseColor", "normal", "specular", "emissive", "metallicRoughness", "occlusion"};
    const gltf_material_info_t *info;
    if (!asset || !material_infos || material_index < 0 || material_index >= asset->material_count)
        return 1;
    info = &material_infos[material_index];
    for (int slot = 0; slot < RT_MATERIAL3D_TEXTURE_SLOT_COUNT; slot++) {
        const gltf_texture_info_t *texture = &info->slots[slot];
        if (!texture->present)
            continue;
        if (texture->texcoord < 0 || texture->texcoord > 1) {
            rt_asset_error_setf(RT_ASSET_ERROR_UNSUPPORTED,
                                "GLTF.Load: mesh %d primitive %d material %lld %s texture "
                                "references unsupported TEXCOORD_%d",
                                mesh_json_index,
                                primitive_index,
                                (long long)material_index,
                                slot_names[slot],
                                texture->texcoord);
            return 0;
        }
        if ((texture->texcoord == 0 && !has_uv0) || (texture->texcoord == 1 && !has_uv1)) {
            rt_asset_error_setf(RT_ASSET_ERROR_CORRUPT,
                                "GLTF.Load: mesh %d primitive %d material %lld %s texture "
                                "requires missing TEXCOORD_%d",
                                mesh_json_index,
                                primitive_index,
                                (long long)material_index,
                                slot_names[slot],
                                texture->texcoord);
            return 0;
        }
    }
    return 1;
}

/**
 * @brief Validate the base and every variant material against one primitive's decoded UV streams.
 * @details KHR_materials_variants mappings can replace the base material after import, so all
 *          valid mapped material indices must satisfy the same stream contract before the mesh is
 *          committed. On failure this helper marks the enclosing load transaction failed.
 * @param asset Borrowed destination asset and material table.
 * @param material_infos Borrowed per-material authored texture metadata.
 * @param prim Borrowed primitive JSON object containing optional variant mappings.
 * @param base_material_index Base primitive material index, or -1 for the generated default.
 * @param has_uv0 Nonzero when TEXCOORD_0 decoded successfully.
 * @param has_uv1 Nonzero when TEXCOORD_1 decoded successfully.
 * @param mesh_json_index Source mesh index for diagnostics.
 * @param primitive_index Source primitive index for diagnostics.
 * @param load_failed_io Optional enclosing transaction failure flag.
 * @return 1 when all reachable materials are compatible, otherwise 0.
 */
static int gltf_mesh_validate_primitive_uv_streams(const rt_gltf_asset *asset,
                                                   const gltf_material_info_t *material_infos,
                                                   void *prim,
                                                   int64_t base_material_index,
                                                   int has_uv0,
                                                   int has_uv1,
                                                   int mesh_json_index,
                                                   int primitive_index,
                                                   int *load_failed_io) {
    void *extensions;
    void *variants_ext;
    void *mappings;
    int64_t mapping_count;
    if (!gltf_mesh_validate_material_uv_streams(asset,
                                                material_infos,
                                                base_material_index,
                                                has_uv0,
                                                has_uv1,
                                                mesh_json_index,
                                                primitive_index))
        goto fail;
    extensions = jget(prim, "extensions");
    variants_ext = extensions ? jget(extensions, "KHR_materials_variants") : NULL;
    mappings = variants_ext ? jarr(variants_ext, "mappings") : NULL;
    mapping_count = jarr_len(mappings);
    for (int64_t mapping_index = 0; mapping_index < mapping_count; mapping_index++) {
        void *mapping = rt_seq_get(mappings, mapping_index);
        int64_t material_index = jint(mapping, "material", -1);
        if (!gltf_mesh_validate_material_uv_streams(asset,
                                                    material_infos,
                                                    material_index,
                                                    has_uv0,
                                                    has_uv1,
                                                    mesh_json_index,
                                                    primitive_index))
            goto fail;
    }
    return 1;
fail:
    if (load_failed_io)
        *load_failed_io = 1;
    return 0;
}

/**
 * @brief Consume and commit a worker-decoded primitive after validating authored UV requirements.
 * @param asset Asset receiving the decoded runtime mesh.
 * @param preload_bundle Bundle containing packed mesh and optional morph dependencies.
 * @param mesh_json_index Source glTF mesh index.
 * @param primitive_index Primitive index within the source mesh.
 * @param prim Primitive JSON object used for variant and UV validation.
 * @param material_index Source base-material index.
 * @param material_infos Per-material authored texture metadata.
 * @param primitive_material Borrowed material assigned to the committed primitive.
 * @param primitive_materials Parallel per-runtime-mesh material table.
 * @param load_failed_io Optional transaction failure flag.
 * @param mesh_idx_io In/out next runtime mesh slot.
 * @return 1 when a decoded mesh was committed, 0 when no matching preload entry exists, and -1
 *         when a decoded entry was consumed but rejected by UV/material validation.
 */
static int gltf_mesh_take_preloaded_primitive(rt_gltf_asset *asset,
                                              rt_gltf_preload_bundle *preload_bundle,
                                              int32_t mesh_json_index,
                                              int32_t primitive_index,
                                              void *prim,
                                              int64_t material_index,
                                              const gltf_material_info_t *material_infos,
                                              void *primitive_material,
                                              void **primitive_materials,
                                              int *load_failed_io,
                                              int32_t *mesh_idx_io) {
    uint32_t decoded_mesh_flags = 0u;
    void *decoded_mesh = gltf_preload_take_decoded_mesh(
        preload_bundle, mesh_json_index, primitive_index, &decoded_mesh_flags);
    if (!decoded_mesh)
        return 0;
    if (!gltf_mesh_validate_primitive_uv_streams(
            asset,
            material_infos,
            prim,
            material_index,
            (decoded_mesh_flags & GLTF_PRELOAD_MESH_POD_HAS_UV0) != 0,
            (decoded_mesh_flags & GLTF_PRELOAD_MESH_POD_HAS_UV1) != 0,
            mesh_json_index,
            primitive_index,
            load_failed_io)) {
        gltf_release_local(decoded_mesh);
        return -1;
    }
    if ((decoded_mesh_flags & GLTF_PRELOAD_MESH_POD_HAS_NORMALS) == 0)
        rt_mesh3d_recalc_normals(decoded_mesh);
    if ((decoded_mesh_flags & GLTF_PRELOAD_MESH_POD_HAS_TANGENTS) == 0 &&
        (decoded_mesh_flags & GLTF_PRELOAD_MESH_POD_HAS_UV0) != 0) {
        rt_material3d *tangent_material = (rt_material3d *)primitive_material;
        if (rt_gltf_active_load_options()->force_tangents ||
            (tangent_material && tangent_material->normal_map))
            rt_mesh3d_calc_tangents(decoded_mesh);
    }
    asset->meshes[*mesh_idx_io] = decoded_mesh;
    (*mesh_idx_io)++;
    asset->mesh_count = *mesh_idx_io;
    if (primitive_materials)
        primitive_materials[*mesh_idx_io - 1] = primitive_material;
    return 1;
}

/// @brief Validate pairing, shape, payload, and length of a primitive's skin accessors.
/// @param views Mutable primitive-view set whose resolved skin streams are inspected.
/// @param vertex_count Number of positions that every present skin stream must cover.
/// @return Non-zero when JOINTS/WEIGHTS pairs are absent or fully valid.
static int gltf_mesh_validate_skin_views(gltf_mesh_primitive_views_t *views, int32_t vertex_count) {
    if (!views)
        return 0;
    if ((views->has_joints != views->has_weights) || (views->has_joints1 != views->has_weights1))
        return 0;
    if (views->has_joints &&
        (!gltf_accessor_has_payload(&views->joints) ||
         !gltf_accessor_has_payload(&views->weights) || !gltf_accessor_is_joints(&views->joints) ||
         !gltf_accessor_is_weights(&views->weights) || views->joints.count < vertex_count ||
         views->weights.count < vertex_count))
        return 0;
    if (views->has_joints1 &&
        (!gltf_accessor_has_payload(&views->joints1) ||
         !gltf_accessor_has_payload(&views->weights1) ||
         !gltf_accessor_is_joints(&views->joints1) || !gltf_accessor_is_weights(&views->weights1) ||
         views->joints1.count < vertex_count || views->weights1.count < vertex_count))
        return 0;
    return 1;
}

/// @brief Resolve and validate all accessors and draw-count metadata for one ordinary primitive.
/// @param root glTF JSON root.
/// @param buffers Materialized glTF buffer table.
/// @param buf_count Number of entries in @p buffers.
/// @param attrs Primitive attribute semantic map.
/// @param prim Primitive JSON object.
/// @param mode glTF primitive topology mode.
/// @param views Destination view set, cleared before resolution.
/// @param load_failed_io Optional hard-failure flag raised for invalid required data.
/// @return Non-zero when positions, optional streams, skin pairs, and triangle count are usable.
static int gltf_mesh_read_primitive_views(void *root,
                                          gltf_buffer_t *buffers,
                                          int32_t buf_count,
                                          void *attrs,
                                          void *prim,
                                          int64_t mode,
                                          gltf_mesh_primitive_views_t *views,
                                          int *load_failed_io) {
    int64_t pos_acc = jint(attrs, "POSITION", -1);
    int64_t norm_acc = jint(attrs, "NORMAL", -1);
    int64_t uv0_acc = jint(attrs, "TEXCOORD_0", -1);
    int64_t uv1_acc = jint(attrs, "TEXCOORD_1", -1);
    int64_t color_acc = jint(attrs, "COLOR_0", -1);
    int64_t tangent_acc = jint(attrs, "TANGENT", -1);
    int64_t joints_acc = jint(attrs, "JOINTS_0", -1);
    int64_t weights_acc = jint(attrs, "WEIGHTS_0", -1);
    int64_t joints1_acc = jint(attrs, "JOINTS_1", -1);
    int64_t weights1_acc = jint(attrs, "WEIGHTS_1", -1);
    int64_t idx_acc = jint(prim, "indices", -1);
    int32_t draw_count;
    if (!views)
        return 0;
    memset(views, 0, sizeof(*views));
    if (pos_acc < 0)
        return 0;

    views->has_normals = gltf_get_accessor_view(root, norm_acc, buffers, buf_count, &views->norm);
    views->has_uv0 = gltf_get_accessor_view(root, uv0_acc, buffers, buf_count, &views->uv0);
    views->has_uv1 = gltf_get_accessor_view(root, uv1_acc, buffers, buf_count, &views->uv1);
    views->has_colors = gltf_get_accessor_view(root, color_acc, buffers, buf_count, &views->color);
    views->has_tangents =
        gltf_get_accessor_view(root, tangent_acc, buffers, buf_count, &views->tangent);
    views->has_joints =
        gltf_get_accessor_view(root, joints_acc, buffers, buf_count, &views->joints);
    views->has_weights =
        gltf_get_accessor_view(root, weights_acc, buffers, buf_count, &views->weights);
    views->has_joints1 =
        gltf_get_accessor_view(root, joints1_acc, buffers, buf_count, &views->joints1);
    views->has_weights1 =
        gltf_get_accessor_view(root, weights1_acc, buffers, buf_count, &views->weights1);
    views->has_indices = gltf_get_accessor_view(root, idx_acc, buffers, buf_count, &views->idx);

    if (!gltf_get_accessor_view(root, pos_acc, buffers, buf_count, &views->pos) ||
        views->pos.count == 0 || !gltf_accessor_has_payload(&views->pos) ||
        !gltf_accessor_is_position(&views->pos)) {
        if (load_failed_io)
            *load_failed_io = 1;
        return 0;
    }
    if (views->has_normals &&
        (!gltf_accessor_has_payload(&views->norm) || !gltf_accessor_is_normal(&views->norm) ||
         views->norm.count < views->pos.count))
        views->has_normals = 0;
    if (views->has_uv0 &&
        (!gltf_accessor_has_payload(&views->uv0) || !gltf_accessor_is_texcoord(&views->uv0) ||
         views->uv0.count < views->pos.count))
        views->has_uv0 = 0;
    if (views->has_uv1 &&
        (!gltf_accessor_has_payload(&views->uv1) || !gltf_accessor_is_texcoord(&views->uv1) ||
         views->uv1.count < views->pos.count))
        views->has_uv1 = 0;
    if (views->has_colors &&
        (!gltf_accessor_has_payload(&views->color) || !gltf_accessor_is_color(&views->color) ||
         views->color.count < views->pos.count))
        views->has_colors = 0;
    if (views->has_tangents &&
        (!gltf_accessor_has_payload(&views->tangent) ||
         !gltf_accessor_is_tangent(&views->tangent, 4) || views->tangent.count < views->pos.count))
        views->has_tangents = 0;
    if (views->has_indices &&
        (!gltf_accessor_has_payload(&views->idx) || !gltf_accessor_is_indices(&views->idx))) {
        if (load_failed_io)
            *load_failed_io = 1;
        return 0;
    }
    if (!gltf_mesh_validate_skin_views(views, views->pos.count)) {
        if (load_failed_io)
            *load_failed_io = 1;
        return 0;
    }

    draw_count = views->has_indices ? views->idx.count : views->pos.count;
    views->triangle_estimate = gltf_primitive_triangle_count(mode, draw_count);
    if (views->triangle_estimate < 0) {
        rt_asset_error_add_import_stat(RT_ASSET_IMPORT_STAT_SKIPPED_PRIMITIVES, 1);
        rt_asset_error_add_warningf("glTF primitive mode %lld is unsupported; primitive skipped",
                                    (long long)mode);
        return 0;
    }
    if (views->triangle_estimate == 0)
        return 0;
    views->has_skin0 = views->has_joints && views->has_weights;
    views->has_skin1 = views->has_joints1 && views->has_weights1;
    return 1;
}

/// @brief Decoded, sanitized vertex attributes before committing them to runtime storage.
typedef struct {
    /// Position vector.
    float pos[3];
    /// Normal vector.
    float nrm[3];
    /// Primary texture coordinates.
    float uv[2];
    /// Secondary texture coordinates.
    float uv1[2];
    /// RGBA vertex color.
    float color[4];
    /// XYZW tangent with handedness in W.
    float tangent[4];
    /// First four skin weights.
    float weights[4];
    /// First four joint indices.
    uint32_t joints[4];
    /// Optional second set of four skin weights.
    float weights1[4];
    /// Optional second set of four joint indices.
    uint32_t joints1[4];
} gltf_mesh_import_vertex_t;

/// @brief Initialize an import vertex with glTF defaults for missing attributes.
/// @param vertex Destination vertex set to zero position/UV/skin, white color, and positive
///               tangent handedness.
static void gltf_mesh_import_vertex_init(gltf_mesh_import_vertex_t *vertex) {
    static const gltf_mesh_import_vertex_t zero_vertex = {0};
    *vertex = zero_vertex;
    vertex->color[0] = 1.0f;
    vertex->color[1] = 1.0f;
    vertex->color[2] = 1.0f;
    vertex->color[3] = 1.0f;
    vertex->tangent[3] = 1.0f;
}

/// @brief Verify every decoded floating-point component required by a primitive is finite.
/// @param views Primitive view flags controlling which skin sets are inspected.
/// @param vertex Decoded import vertex.
/// @return Non-zero when position, display attributes, and present skin weights are all finite.
static int gltf_mesh_import_vertex_is_finite(const gltf_mesh_primitive_views_t *views,
                                             const gltf_mesh_import_vertex_t *vertex) {
    return gltf_f32_array_is_finite(vertex->pos, 3) && gltf_f32_array_is_finite(vertex->nrm, 3) &&
           gltf_f32_array_is_finite(vertex->uv, 2) && gltf_f32_array_is_finite(vertex->uv1, 2) &&
           gltf_f32_array_is_finite(vertex->color, 4) &&
           gltf_f32_array_is_finite(vertex->tangent, 4) &&
           (!views->has_skin0 || gltf_f32_array_is_finite(vertex->weights, 4)) &&
           (!views->has_skin1 || gltf_f32_array_is_finite(vertex->weights1, 4));
}

/// @brief Merge two four-influence skin sets into the strongest valid four influences.
/// @param vertex Import vertex updated in place.
/// @param has_primary_skin Non-zero when the first influence set should participate.
/// @param truncated_io Optional flag raised when meaningful influences are discarded.
/// @param out_of_range_io Optional flag raised for weighted joints outside @p joint_limit.
/// @param joint_limit Exclusive maximum accepted joint index.
static void gltf_mesh_merge_extra_joint_influences(gltf_mesh_import_vertex_t *vertex,
                                                   int has_primary_skin,
                                                   int *truncated_io,
                                                   int *out_of_range_io,
                                                   uint32_t joint_limit) {
    uint32_t merged_joints[4] = {0u, 0u, 0u, 0u};
    float merged_weights[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    if (has_primary_skin) {
        for (int j = 0; j < 4; j++)
            gltf_add_top_joint_influence_limit(vertex->joints[j],
                                               vertex->weights[j],
                                               merged_joints,
                                               merged_weights,
                                               truncated_io,
                                               out_of_range_io,
                                               joint_limit);
    }
    for (int j = 0; j < 4; j++)
        gltf_add_top_joint_influence_limit(vertex->joints1[j],
                                           vertex->weights1[j],
                                           merged_joints,
                                           merged_weights,
                                           truncated_io,
                                           out_of_range_io,
                                           joint_limit);
    gltf_normalize_joint_influences(merged_weights);
    memcpy(vertex->joints, merged_joints, sizeof(vertex->joints));
    memcpy(vertex->weights, merged_weights, sizeof(vertex->weights));
}

/// @brief Remove weighted primary influences whose joint indices exceed a runtime limit.
/// @param vertex Import vertex updated in place.
/// @param out_of_range_io Optional flag raised when a meaningful influence is removed.
/// @param joint_limit Exclusive maximum accepted joint index.
static void gltf_mesh_clip_joint_influences(gltf_mesh_import_vertex_t *vertex,
                                            int *out_of_range_io,
                                            uint32_t joint_limit) {
    for (int j = 0; j < 4; j++) {
        if (vertex->joints[j] >= joint_limit) {
            if (out_of_range_io && isfinite(vertex->weights[j]) && vertex->weights[j] > 1e-8f)
                *out_of_range_io = 1;
            vertex->joints[j] = 0u;
            vertex->weights[j] = 0.0f;
        }
    }
}

/// @brief Decode and sanitize one logical vertex from resolved primitive accessors.
/// @param views Resolved primitive attribute views.
/// @param vertex_index Logical accessor element index.
/// @param vertex Destination import vertex.
/// @param invalid_authored_normals_io Optional flag raised for degenerate authored normals.
/// @param invalid_authored_tangents_io Optional flag raised for invalid authored tangents.
/// @param skin_stats_io Optional accumulated skin truncation/range statistics.
/// @param joint_limit Exclusive accepted joint-index limit.
/// @return Non-zero when all required decoded floating-point data is finite.
static int gltf_mesh_read_import_vertex(const gltf_mesh_primitive_views_t *views,
                                        int32_t vertex_index,
                                        gltf_mesh_import_vertex_t *vertex,
                                        int *invalid_authored_normals_io,
                                        int *invalid_authored_tangents_io,
                                        gltf_skin_import_stats_t *skin_stats_io,
                                        uint32_t joint_limit) {
    gltf_mesh_import_vertex_init(vertex);
    gltf_accessor_read_f32(&views->pos, vertex_index, vertex->pos, 3);
    if (views->has_normals)
        gltf_accessor_read_f32(&views->norm, vertex_index, vertex->nrm, 3);
    if (views->has_uv0)
        gltf_accessor_read_f32(&views->uv0, vertex_index, vertex->uv, 2);
    if (views->has_uv1)
        gltf_accessor_read_f32(&views->uv1, vertex_index, vertex->uv1, 2);
    else {
        vertex->uv1[0] = vertex->uv[0];
        vertex->uv1[1] = vertex->uv[1];
    }
    if (views->has_colors) {
        gltf_accessor_read_f32(&views->color, vertex_index, vertex->color, 4);
        if (views->color.comp_count < 4)
            vertex->color[3] = 1.0f;
    }
    if (views->has_tangents) {
        gltf_accessor_read_f32(&views->tangent, vertex_index, vertex->tangent, 4);
        if (views->tangent.comp_count < 4)
            vertex->tangent[3] = 1.0f;
    }
    if (views->has_joints)
        gltf_accessor_read_u32(&views->joints, vertex_index, vertex->joints, 4);
    if (views->has_weights)
        gltf_accessor_read_f32(&views->weights, vertex_index, vertex->weights, 4);
    if (views->has_joints1)
        gltf_accessor_read_u32(&views->joints1, vertex_index, vertex->joints1, 4);
    if (views->has_weights1)
        gltf_accessor_read_f32(&views->weights1, vertex_index, vertex->weights1, 4);
    if (!gltf_mesh_import_vertex_is_finite(views, vertex))
        return 0;
    if (views->has_normals && !gltf_normalize_vec3f_in_place(vertex->nrm)) {
        if (invalid_authored_normals_io)
            *invalid_authored_normals_io = 1;
        vertex->nrm[0] = 0.0f;
        vertex->nrm[1] = 0.0f;
        vertex->nrm[2] = 0.0f;
    }
    if (views->has_tangents && !gltf_sanitize_tangent4(vertex->tangent)) {
        if (invalid_authored_tangents_io)
            *invalid_authored_tangents_io = 1;
        vertex->tangent[0] = 1.0f;
        vertex->tangent[1] = 0.0f;
        vertex->tangent[2] = 0.0f;
        vertex->tangent[3] = 1.0f;
    }
    if (views->has_skin1 && !(rt_gltf_active_load_options()->eight_bone_influences &&
                              joint_limit == (uint32_t)VGFX3D_MAX_BONES)) {
        int truncated = 0;
        int out_of_range = 0;
        gltf_mesh_merge_extra_joint_influences(
            vertex, views->has_skin0, &truncated, &out_of_range, joint_limit);
        if (skin_stats_io) {
            if (truncated)
                skin_stats_io->truncated_vertices++;
            if (out_of_range)
                skin_stats_io->out_of_range_vertices++;
        }
    } else if (views->has_skin1) {
        /* Eight-influence import: both sets stay raw; the apply step distributes
         * the strongest four into the vertex record and the next four into the
         * mesh's extra-influence side stream. */
    } else if (views->has_skin0) {
        int out_of_range = 0;
        gltf_mesh_clip_joint_influences(vertex, &out_of_range, joint_limit);
        gltf_normalize_joint_influences(vertex->weights);
        if (skin_stats_io && out_of_range)
            skin_stats_io->out_of_range_vertices++;
    }
    return 1;
}

/// @brief Store a vertex's four primary skin influences on a mesh.
/// @details Updates the mesh bone count to include every referenced in-range joint
///          whose normalized weight is significant.
/// @param[in,out] mesh Destination mesh receiving the vertex skin data.
/// @param vertex_index Zero-based destination vertex index.
/// @param[in] vertex Imported vertex containing joint indices and weights.
static void gltf_mesh_apply_vertex_skin(rt_mesh3d *mesh,
                                        int32_t vertex_index,
                                        const gltf_mesh_import_vertex_t *vertex) {
    rt_mesh3d_set_bone_weights(mesh,
                               vertex_index,
                               (int64_t)vertex->joints[0],
                               vertex->weights[0],
                               (int64_t)vertex->joints[1],
                               vertex->weights[1],
                               (int64_t)vertex->joints[2],
                               vertex->weights[2],
                               (int64_t)vertex->joints[3],
                               vertex->weights[3]);
    for (int j = 0; j < 4; j++) {
        if (vertex->weights[j] > 0.0001f && vertex->joints[j] < VGFX3D_MAX_BONES &&
            (int32_t)(vertex->joints[j] + 1u) > mesh->bone_count) {
            mesh->bone_count = (int32_t)(vertex->joints[j] + 1u);
        }
    }
}

/// @brief Distribute up to eight influences across the vertex record (strongest
///        four) and the mesh's extra-influence side stream (next four), with the
///        combined weight set normalized to sum 1.
/// @param[in,out] mesh Destination mesh receiving primary and extra influences.
/// @param vertex_index Zero-based destination vertex index.
/// @param[in] vertex Imported vertex containing both four-influence source sets.
/// @param[in,out] skin_stats_io Optional counters updated when influences are
///                              truncated or reference out-of-range joints.
static void gltf_mesh_apply_vertex_skin8(rt_mesh3d *mesh,
                                         int32_t vertex_index,
                                         const gltf_mesh_import_vertex_t *vertex,
                                         gltf_skin_import_stats_t *skin_stats_io) {
    uint32_t joints[8];
    float weights[8];
    int count = 0;
    int truncated = 0;
    int out_of_range = 0;
    float sum = 0.0f;
    /* Merge the two source sets, deduplicating joints and dropping out-of-range
     * or negligible influences. */
    for (int set = 0; set < 2; set++) {
        const uint32_t *sj = set == 0 ? vertex->joints : vertex->joints1;
        const float *sw = set == 0 ? vertex->weights : vertex->weights1;
        for (int j = 0; j < 4; j++) {
            float w = sw[j];
            int found = -1;
            if (!isfinite(w) || w <= 1e-8f)
                continue;
            if (sj[j] >= VGFX3D_MAX_BONES) {
                out_of_range = 1;
                continue;
            }
            for (int q = 0; q < count; q++) {
                if (joints[q] == sj[j]) {
                    found = q;
                    break;
                }
            }
            if (found >= 0) {
                weights[found] += w;
            } else if (count < 8) {
                joints[count] = sj[j];
                weights[count] = w;
                count++;
            } else {
                /* ninth meaningful influence: evict the weakest if smaller */
                int weakest = 0;
                for (int q = 1; q < 8; q++) {
                    if (weights[q] < weights[weakest])
                        weakest = q;
                }
                truncated = 1;
                if (w > weights[weakest]) {
                    joints[weakest] = sj[j];
                    weights[weakest] = w;
                }
            }
        }
    }
    /* Sort descending by weight (insertion sort over at most 8 entries). */
    for (int i = 1; i < count; i++) {
        uint32_t jt = joints[i];
        float wt = weights[i];
        int k = i - 1;
        while (k >= 0 && weights[k] < wt) {
            joints[k + 1] = joints[k];
            weights[k + 1] = weights[k];
            k--;
        }
        joints[k + 1] = jt;
        weights[k + 1] = wt;
    }
    for (int i = 0; i < count; i++)
        sum += weights[i];
    if (sum > 1e-8f) {
        for (int i = 0; i < count; i++)
            weights[i] /= sum;
    }
    rt_mesh3d_set_bone_weights(mesh,
                               vertex_index,
                               count > 0 ? (int64_t)joints[0] : 0,
                               count > 0 ? weights[0] : 0.0,
                               count > 1 ? (int64_t)joints[1] : 0,
                               count > 1 ? weights[1] : 0.0,
                               count > 2 ? (int64_t)joints[2] : 0,
                               count > 2 ? weights[2] : 0.0,
                               count > 3 ? (int64_t)joints[3] : 0,
                               count > 3 ? weights[3] : 0.0);
    if (mesh->extra_influences && vertex_index >= 0 &&
        (uint32_t)vertex_index < mesh->vertex_count) {
        /* rt_mesh3d_set_bone_weights renormalizes the four vertex weights to sum
         * 1, so scale the side-stream weights by the same factor; the skinner
         * divides by the combined weight, preserving the authored proportions. */
        vgfx3d_extra_influences_t *extra = &mesh->extra_influences[vertex_index];
        float primary_sum = 0.0f;
        int primary_count = count < 4 ? count : 4;
        for (int i = 0; i < primary_count; i++)
            primary_sum += weights[i];
        memset(extra, 0, sizeof(*extra));
        for (int i = 4; i < count; i++) {
            extra->indices[i - 4] = (uint16_t)joints[i];
            extra->weights[i - 4] = primary_sum > 1e-8f ? weights[i] / primary_sum : weights[i];
        }
    }
    for (int i = 0; i < count; i++) {
        if (weights[i] > 0.0001f && (int32_t)(joints[i] + 1u) > mesh->bone_count)
            mesh->bone_count = (int32_t)(joints[i] + 1u);
    }
    if (skin_stats_io) {
        if (truncated)
            skin_stats_io->truncated_vertices++;
        if (out_of_range)
            skin_stats_io->out_of_range_vertices++;
    }
}

/// @brief Decode and append every vertex described by a primitive's accessor views.
/// @details Imports geometry attributes and applies either four- or eight-influence
///          skinning data according to the active load options and available streams.
/// @param[in,out] mesh Destination mesh whose geometry batch is already open.
/// @param[in] views Validated accessor views and semantic-presence flags.
/// @param[in,out] invalid_authored_normals_io Optional flag raised for invalid normals.
/// @param[in,out] invalid_authored_tangents_io Optional flag raised for invalid tangents.
/// @param[in,out] skin_stats_io Optional accumulated skin import statistics.
/// @return Non-zero if every vertex was decoded and appended; zero on invalid
///         floating-point data or mesh-storage failure.
static int gltf_mesh_import_vertices(rt_mesh3d *mesh,
                                     const gltf_mesh_primitive_views_t *views,
                                     int *invalid_authored_normals_io,
                                     int *invalid_authored_tangents_io,
                                     gltf_skin_import_stats_t *skin_stats_io) {
    for (int32_t vi = 0; vi < views->pos.count; vi++) {
        gltf_mesh_import_vertex_t vertex;
        if (!gltf_mesh_read_import_vertex(views,
                                          vi,
                                          &vertex,
                                          invalid_authored_normals_io,
                                          invalid_authored_tangents_io,
                                          skin_stats_io,
                                          VGFX3D_MAX_BONES))
            return 0;
        if (!gltf_mesh_append_import_vertex(
                mesh, vertex.pos, vertex.nrm, vertex.uv, vertex.uv1, vertex.color, vertex.tangent))
            return 0;
        if (views->has_skin1 && mesh->extra_influences)
            gltf_mesh_apply_vertex_skin8(mesh, vi, &vertex, skin_stats_io);
        else if (views->has_skin0 || views->has_skin1)
            gltf_mesh_apply_vertex_skin(mesh, vi, &vertex);
    }
    return 1;
}

/// @brief Finish a conventionally decoded primitive after its vertices are imported.
/// @details Emits topology, closes the geometry batch, regenerates invalid or absent
///          derived attributes when appropriate, and imports morph targets.
/// @param[in] asset Asset providing material bounds and active material objects.
/// @param[in] root Parsed glTF document root used by morph-target import.
/// @param[in] buffers Resolved glTF buffer table.
/// @param buf_count Number of entries in @p buffers.
/// @param[in] mesh_json Parsed parent mesh object.
/// @param[in] prim Parsed primitive object.
/// @param[in,out] mesh Destination mesh whose geometry batch is open.
/// @param mode glTF primitive topology mode.
/// @param[in] views Validated primitive accessor views.
/// @param material_idx Primitive material index, or a negative value when absent.
/// @param[in] primitive_material Resolved runtime material, if any.
/// @param invalid_authored_normals Non-zero when authored normals must be regenerated.
/// @param invalid_authored_tangents Non-zero when authored tangents must be regenerated.
/// @param[out] load_failed_io Optional flag raised on a hard mesh-storage failure.
/// @return Non-zero when valid indices produced geometry; zero when the primitive
///         must be discarded or a hard failure occurred.
static int gltf_mesh_finalize_imported_primitive(rt_gltf_asset *asset,
                                                 void *root,
                                                 gltf_buffer_t *buffers,
                                                 int32_t buf_count,
                                                 void *mesh_json,
                                                 void *prim,
                                                 void *mesh,
                                                 int64_t mode,
                                                 const gltf_mesh_primitive_views_t *views,
                                                 int64_t material_idx,
                                                 void *primitive_material,
                                                 int invalid_authored_normals,
                                                 int invalid_authored_tangents,
                                                 int *load_failed_io) {
    if (!gltf_append_primitive_indices(
            mesh, mode, views->has_indices ? &views->idx : NULL, views->pos.count)) {
        rt_mesh3d_end_geometry_batch((rt_mesh3d *)mesh);
        if (((rt_mesh3d *)mesh)->build_failed) {
            /* Storage growth failed mid-append: a hard load failure. */
            if (load_failed_io)
                *load_failed_io = 1;
        } else {
            /* Every triangle was dropped (out-of-range or degenerate indices):
             * skip the primitive with a diagnostic instead of failing the asset. */
            rt_asset_error_add_import_stat(RT_ASSET_IMPORT_STAT_SKIPPED_PRIMITIVES, 1);
            rt_asset_error_add_warning(
                "glTF primitive skipped: its indices form no valid triangles");
        }
        return 0;
    }
    rt_mesh3d_end_geometry_batch((rt_mesh3d *)mesh);
    if ((!views->has_normals || invalid_authored_normals) && ((rt_mesh3d *)mesh)->vertex_count > 0)
        rt_mesh3d_recalc_normals(mesh);
    if ((!views->has_tangents || invalid_authored_tangents) && views->has_uv0 &&
        material_idx >= 0 && material_idx < asset->material_count) {
        rt_material3d *tangent_material = (rt_material3d *)primitive_material;
        if (rt_gltf_active_load_options()->force_tangents ||
            (tangent_material && tangent_material->normal_map))
            rt_mesh3d_calc_tangents(mesh);
    }
    gltf_import_primitive_morph_targets(
        root, buffers, buf_count, mesh_json, prim, mesh, views->pos.count);
    return 1;
}

/// @brief Shared state used while expanding glTF mesh primitives into asset mesh slots.
typedef struct {
    /// Asset receiving decoded mesh objects and mesh-count updates.
    rt_gltf_asset *asset;
    /// Per-material UV and feature metadata used during primitive validation.
    const gltf_material_info_t *material_infos;
    /// Parsed glTF document root.
    void *root;
    /// Resolved buffer table addressed by accessor and extension data.
    gltf_buffer_t *buffers;
    /// Number of entries in @ref buffers.
    int buf_count;
    /// Optional asynchronously decoded primitive bundle.
    rt_gltf_preload_bundle *preload_bundle;
    /// Number of material slots accepted by the primitive material resolver.
    int material_capacity;
    /// Per-output-mesh resolved material table.
    void **primitive_materials;
    /// Per-output-mesh KHR_materials_variants mapping table.
    int32_t **primitive_variant_mappings;
    /// Address of the lazily created fallback material storage.
    void **default_material_io;
    /// Optional flag raised when a hard import failure occurs.
    int *load_failed_io;
    /// Current capacity of the per-mesh-slot tables; may grow when oversized
    /// skins partition into additional sub-meshes.
    int primitive_table_capacity;
} gltf_mesh_load_context_t;

/// @brief Ensure the asset mesh array and the per-mesh-slot side tables can hold
///        @p extra additional meshes beyond the current asset->mesh_count.
/// @details Partitioned skins are the only path that outgrows the pre-counted
///          primitive total, so this stays a no-op for ordinary assets.
/// @param[in,out] ctx Mesh-load context owning the arrays that may be reallocated.
/// @param extra Number of additional mesh slots required.
/// @return Non-zero when the current or expanded capacity is sufficient; zero on
///         integer overflow or allocation failure.
static int gltf_mesh_grow_output_tables(gltf_mesh_load_context_t *ctx, int extra) {
    rt_gltf_asset *asset;
    int needed;
    int old_cap;
    int new_cap;
    void **new_meshes;
    void **new_materials;
    int32_t **new_variant_mappings;
    if (!ctx || !ctx->asset)
        return 0;
    asset = ctx->asset;
    if (extra <= 0)
        return extra == 0;
    old_cap = ctx->primitive_table_capacity;
    if (old_cap < 0 || asset->mesh_count < 0 || asset->mesh_count > old_cap ||
        asset->mesh_capacity < old_cap ||
        (old_cap > 0 &&
         (!asset->meshes || !ctx->primitive_materials || !ctx->primitive_variant_mappings)) ||
        asset->mesh_count > INT32_MAX - extra)
        return 0;
    needed = asset->mesh_count + extra;
    if (needed <= old_cap)
        return 1;
    new_cap = old_cap > INT32_MAX / 2 ? needed : old_cap * 2;
    if (new_cap < needed)
        new_cap = needed;
    if ((size_t)new_cap > SIZE_MAX / sizeof(void *) ||
        (size_t)new_cap > SIZE_MAX / sizeof(int32_t *))
        return 0;

    /* The three arrays form one logical table. Allocate every replacement
     * before publishing any pointer so an allocation failure cannot leave the
     * asset capacity ahead of one or both side tables. */
    new_meshes = (void **)calloc((size_t)new_cap, sizeof(*new_meshes));
    new_materials = (void **)calloc((size_t)new_cap, sizeof(*new_materials));
    new_variant_mappings = (int32_t **)calloc((size_t)new_cap, sizeof(*new_variant_mappings));
    if (!new_meshes || !new_materials || !new_variant_mappings) {
        free(new_meshes);
        free(new_materials);
        free(new_variant_mappings);
        return 0;
    }
    if (old_cap > 0) {
        memcpy(new_meshes, asset->meshes, (size_t)old_cap * sizeof(*new_meshes));
        memcpy(new_materials, ctx->primitive_materials, (size_t)old_cap * sizeof(*new_materials));
        memcpy(new_variant_mappings,
               ctx->primitive_variant_mappings,
               (size_t)old_cap * sizeof(*new_variant_mappings));
    }
    free(asset->meshes);
    free(ctx->primitive_materials);
    free(ctx->primitive_variant_mappings);
    asset->meshes = new_meshes;
    asset->mesh_capacity = new_cap;
    ctx->primitive_materials = new_materials;
    ctx->primitive_variant_mappings = new_variant_mappings;
    ctx->primitive_table_capacity = new_cap;
    return 1;
}

/// @brief Parse a primitive's `extensions.KHR_materials_variants.mappings` into a
///        variant-indexed material-index table (-1 = variant unmapped for this primitive).
/// @param[in] asset Asset providing the variant and material table bounds.
/// @param[in] prim Parsed glTF primitive object.
/// @return Malloc-owned table sized asset->variant_name_count, or NULL when the asset has
///         no variants, the primitive has no usable mappings, or on allocation failure.
static int32_t *gltf_parse_primitive_variant_mapping(const rt_gltf_asset *asset, void *prim) {
    void *extensions = jget(prim, "extensions");
    void *variants_ext = extensions ? jget(extensions, "KHR_materials_variants") : NULL;
    void *mappings = variants_ext ? jarr(variants_ext, "mappings") : NULL;
    int64_t mapping_count = jarr_len(mappings);
    int32_t *table;
    int any_mapped = 0;
    if (!asset || asset->variant_name_count <= 0 || mapping_count <= 0)
        return NULL;
    table = (int32_t *)malloc((size_t)asset->variant_name_count * sizeof(int32_t));
    if (!table)
        return NULL;
    for (int32_t v = 0; v < asset->variant_name_count; v++)
        table[v] = -1;
    for (int64_t mi = 0; mi < mapping_count; mi++) {
        void *entry = rt_seq_get(mappings, mi);
        int64_t material_idx = jint(entry, "material", -1);
        void *variant_refs = jarr(entry, "variants");
        if (material_idx < 0 || material_idx >= asset->material_count)
            continue;
        for (int64_t vi = 0; vi < jarr_len(variant_refs); vi++) {
            int64_t variant_idx = jvalue_int(rt_seq_get(variant_refs, vi), -1);
            if (variant_idx >= 0 && variant_idx < asset->variant_name_count) {
                table[variant_idx] = (int32_t)material_idx;
                any_mapped = 1;
            }
        }
    }
    if (!any_mapped) {
        free(table);
        return NULL;
    }
    return table;
}

/// @brief Parse and record the variant mapping for the primitive stored at @p mesh_slot.
/// @param[in,out] ctx Mesh-load context owning the variant side table.
/// @param[in] prim Parsed glTF primitive object.
/// @param mesh_slot Zero-based output mesh slot to receive the owned mapping table.
static void gltf_mesh_store_primitive_variant_mapping(gltf_mesh_load_context_t *ctx,
                                                      void *prim,
                                                      int mesh_slot) {
    if (!ctx || !ctx->primitive_variant_mappings || mesh_slot < 0)
        return;
    ctx->primitive_variant_mappings[mesh_slot] =
        gltf_parse_primitive_variant_mapping(ctx->asset, prim);
}

/// @brief First of two passes over the glTF mesh array: sum every mesh's primitive count so the
/// output tables (mesh_prim_start/_count, primitive_materials) can be allocated once at the exact
/// total size. This deliberate pre-count trades a cheap O(meshes) walk of jarr_len() for avoiding
/// per-primitive reallocs and fragmentation during the far more expensive decode pass — the count
/// cost is negligible relative to vertex/index decoding, so the two-pass structure is intentional.
/// @param[in] meshes_arr Parsed glTF `meshes` array.
/// @param mesh_json_count Number of mesh objects to inspect.
/// @param[out] out_total_prims Receives the sum of all primitive-array lengths.
/// @param[out] load_failed_io Optional flag raised if the sum exceeds signed
///                            32-bit storage limits.
/// @return Non-zero when the exact primitive count was produced; zero on overflow.
static int gltf_mesh_count_total_primitives(void *meshes_arr,
                                            int mesh_json_count,
                                            int *out_total_prims,
                                            int *load_failed_io) {
    int total_prims = 0;
    for (int i = 0; i < mesh_json_count; i++) {
        void *mesh_json = rt_seq_get(meshes_arr, (int64_t)i);
        void *prims = jarr(mesh_json, "primitives");
        int64_t prim_len = jarr_len(prims);
        if (prim_len > INT32_MAX || total_prims > INT32_MAX - prim_len) {
            if (load_failed_io)
                *load_failed_io = 1;
            return 0;
        }
        total_prims += (int)prim_len;
    }
    *out_total_prims = total_prims;
    return 1;
}

/// @brief Allocate the mesh-range and per-primitive output tables for mesh import.
/// @details All returned tables are zero-initialized. On failure, every allocation
///          made here is released and the asset mesh table is reset.
/// @param[in,out] asset Asset whose mesh pointer table is allocated.
/// @param mesh_json_count Number of glTF mesh objects requiring range entries.
/// @param total_prims Pre-counted total number of glTF primitives.
/// @param[out] out_mesh_prim_start Receives the per-glTF-mesh first output slot table.
/// @param[out] out_mesh_prim_count Receives the per-glTF-mesh output count table.
/// @param[out] out_primitive_materials Receives the per-output-mesh material table.
/// @param[out] out_primitive_variant_mappings Receives the per-output-mesh variant tables.
/// @param[out] load_failed_io Optional flag raised on allocation failure.
/// @return Non-zero when every output table was allocated; zero otherwise.
static int gltf_mesh_alloc_output_tables(rt_gltf_asset *asset,
                                         int mesh_json_count,
                                         int total_prims,
                                         int **out_mesh_prim_start,
                                         int **out_mesh_prim_count,
                                         void ***out_primitive_materials,
                                         int32_t ***out_primitive_variant_mappings,
                                         int *load_failed_io) {
    int *mesh_prim_start = (int *)calloc((size_t)mesh_json_count, sizeof(int));
    int *mesh_prim_count = (int *)calloc((size_t)mesh_json_count, sizeof(int));
    void **primitive_materials = (void **)calloc((size_t)total_prims, sizeof(void *));
    int32_t **primitive_variant_mappings =
        (int32_t **)calloc((size_t)total_prims, sizeof(int32_t *));
    asset->meshes = (void **)calloc((size_t)total_prims, sizeof(void *));
    asset->mesh_capacity = asset->meshes ? total_prims : 0;
    if (!mesh_prim_start || !mesh_prim_count || !primitive_materials ||
        !primitive_variant_mappings || !asset->meshes) {
        free(mesh_prim_start);
        free(mesh_prim_count);
        free(primitive_materials);
        free(primitive_variant_mappings);
        free(asset->meshes);
        asset->meshes = NULL;
        asset->mesh_capacity = 0;
        if (load_failed_io)
            *load_failed_io = 1;
        return 0;
    }
    *out_mesh_prim_start = mesh_prim_start;
    *out_mesh_prim_count = mesh_prim_count;
    *out_primitive_materials = primitive_materials;
    *out_primitive_variant_mappings = primitive_variant_mappings;
    return 1;
}

/// @brief Decode and append a primitive using `KHR_draco_mesh_compression`.
/// @param[in,out] ctx Shared mesh-load state and output tables.
/// @param[in] mesh_json Parsed parent mesh object.
/// @param[in] prim Parsed primitive object.
/// @param[in] draco_ext Parsed `KHR_draco_mesh_compression` extension object.
/// @param mesh_json_index Zero-based parent mesh index used in diagnostics.
/// @param primitive_index Zero-based primitive index used in diagnostics.
/// @param[in] prim_material Resolved runtime material for the primitive.
/// @param[in,out] mesh_idx_io Next free asset mesh slot, advanced on success.
static void gltf_draco_import_primitive(gltf_mesh_load_context_t *ctx,
                                        void *mesh_json,
                                        void *prim,
                                        void *draco_ext,
                                        int mesh_json_index,
                                        int primitive_index,
                                        void *prim_material,
                                        int *mesh_idx_io);

/// @brief Highest joint index carrying meaningful weight across a primitive's
///        JOINTS_0/JOINTS_1 accessors (-1 when the primitive is unskinned).
/// @param[in] views Primitive accessor views containing any skin streams.
/// @return Highest joint index with a finite, non-negligible weight, or -1 when
///         no meaningful skin influence is present.
static int32_t gltf_mesh_scan_max_joint(const gltf_mesh_primitive_views_t *views) {
    int32_t max_joint = -1;
    if (!views->has_skin0 && !views->has_skin1)
        return -1;
    for (int32_t vi = 0; vi < views->pos.count; vi++) {
        uint32_t joints[4];
        float weights[4];
        if (views->has_skin0) {
            gltf_accessor_read_u32(&views->joints, vi, joints, 4);
            gltf_accessor_read_f32(&views->weights, vi, weights, 4);
            for (int j = 0; j < 4; j++) {
                if (isfinite(weights[j]) && weights[j] > 1e-8f && (int32_t)joints[j] > max_joint)
                    max_joint = (int32_t)joints[j];
            }
        }
        if (views->has_skin1) {
            gltf_accessor_read_u32(&views->joints1, vi, joints, 4);
            gltf_accessor_read_f32(&views->weights1, vi, weights, 4);
            for (int j = 0; j < 4; j++) {
                if (isfinite(weights[j]) && weights[j] > 1e-8f && (int32_t)joints[j] > max_joint)
                    max_joint = (int32_t)joints[j];
            }
        }
    }
    return max_joint;
}

/// @brief Should this skinned primitive take the partitioning path?
/// @details True only when the meaningful joint set exceeds the draw palette AND
///          the document declares a skin large enough to own those joints —
///          out-of-range garbage (joints past every skin) keeps the historical
///          clip-and-warn behavior instead of manufacturing phantom palettes.
/// @param[in] root Parsed glTF document root containing the skin declarations.
/// @param[in] views Primitive accessor views containing any joint streams.
/// @return Non-zero when the primitive should be divided into bone-palette
///         sub-meshes; zero when normal import or clipping should be used.
static int gltf_mesh_primitive_needs_partition(void *root,
                                               const gltf_mesh_primitive_views_t *views) {
    int32_t max_joint = gltf_mesh_scan_max_joint(views);
    void *skins = jarr(root, "skins");
    int64_t skin_count = jarr_len(skins);
    int64_t largest_skin = 0;
    if (max_joint < VGFX3D_MAX_BONES)
        return 0;
    for (int64_t i = 0; i < skin_count; i++) {
        int64_t joints = jarr_len(jarr(rt_seq_get(skins, i), "joints"));
        if (joints > largest_skin)
            largest_skin = joints;
    }
    return (int64_t)max_joint < largest_skin && largest_skin <= (int64_t)VGFX3D_MAX_SKELETON_BONES;
}

/// @brief Import a skinned primitive whose joint set exceeds the 256-slot draw
///        palette by splitting its triangles into sub-meshes whose bone sets fit.
/// @details Standard skin-partitioning: triangles are bucketed greedily; each
///          bucket becomes an independent Mesh3D whose vertex bone indices are
///          bucket-local and whose `bone_map` translates palette slots back to
///          skeleton bone indices at draw time. Morph targets cannot survive the
///          vertex reordering and are skipped with a warning.
/// @param[in,out] ctx Shared mesh-load state and growable output tables.
/// @param[in] prim Parsed primitive object.
/// @param[in] views Validated source accessor views.
/// @param mesh_json_index Zero-based parent mesh index used in diagnostics.
/// @param primitive_index Zero-based primitive index used in diagnostics.
/// @param mode glTF triangle topology mode.
/// @param material_idx Primitive material index, or a negative value when absent.
/// @param[in] prim_material Resolved runtime material for each emitted sub-mesh.
/// @param[in,out] mesh_idx_io Next free asset mesh slot, advanced for each
///                            successfully emitted partition.
static void gltf_mesh_import_partitioned_primitive(gltf_mesh_load_context_t *ctx,
                                                   void *prim,
                                                   const gltf_mesh_primitive_views_t *views,
                                                   int mesh_json_index,
                                                   int primitive_index,
                                                   int64_t mode,
                                                   int64_t material_idx,
                                                   void *prim_material,
                                                   int *mesh_idx_io) {
    int32_t vertex_count = views->pos.count;
    gltf_mesh_import_vertex_t *raw = NULL;
    uint32_t *tris = NULL; /* flat triangle list (source vertex indices) */
    int32_t tri_count = 0;
    int32_t *bucket_of_tri = NULL;
    int32_t bucket_count = 0;
    int32_t *joint_bucket = NULL; /* per-joint last-seen bucket id */
    int32_t *joint_local = NULL;  /* per-joint local slot for the current bucket */
    int32_t *vertex_local = NULL; /* per-vertex local index for the current bucket */
    int invalid_authored_normals = 0;
    int invalid_authored_tangents = 0;
    int emitted_meshes = 0;
    gltf_skin_import_stats_t skin_stats = {0u, 0u};
    if (vertex_count <= 0)
        return;

    raw = (gltf_mesh_import_vertex_t *)malloc((size_t)vertex_count * sizeof(*raw));
    if (!raw)
        goto fail;
    for (int32_t vi = 0; vi < vertex_count; vi++) {
        if (!gltf_mesh_read_import_vertex(views,
                                          vi,
                                          &raw[vi],
                                          &invalid_authored_normals,
                                          &invalid_authored_tangents,
                                          &skin_stats,
                                          VGFX3D_MAX_SKELETON_BONES))
            goto fail;
    }

    /* Expand the primitive into a flat triangle list (same modes as
     * gltf_append_primitive_indices; out-of-range indices drop the triangle). */
    {
        const gltf_accessor_view_t *index_view = views->has_indices ? &views->idx : NULL;
        int32_t count = index_view ? index_view->count : vertex_count;
        int32_t max_tris = count;
        if (count < 3)
            goto fail;
        tris = (uint32_t *)malloc((size_t)max_tris * 3u * sizeof(uint32_t));
        if (!tris)
            goto fail;
        if (mode == 4) {
            for (int32_t i = 0; i + 2 < count; i += 3) {
                uint32_t a = gltf_read_index(index_view, i);
                uint32_t b = gltf_read_index(index_view, i + 1);
                uint32_t c = gltf_read_index(index_view, i + 2);
                if (a >= (uint32_t)vertex_count || b >= (uint32_t)vertex_count ||
                    c >= (uint32_t)vertex_count || a == b || b == c || a == c)
                    continue;
                tris[tri_count * 3 + 0] = a;
                tris[tri_count * 3 + 1] = b;
                tris[tri_count * 3 + 2] = c;
                tri_count++;
            }
        } else if (mode == 5) {
            for (int32_t i = 0; i + 2 < count; i++) {
                uint32_t a = gltf_read_index(index_view, i);
                uint32_t b = gltf_read_index(index_view, i + 1);
                uint32_t c = gltf_read_index(index_view, i + 2);
                if ((i & 1) != 0) {
                    uint32_t tmp = a;
                    a = b;
                    b = tmp;
                }
                if (a >= (uint32_t)vertex_count || b >= (uint32_t)vertex_count ||
                    c >= (uint32_t)vertex_count || a == b || b == c || a == c)
                    continue;
                tris[tri_count * 3 + 0] = a;
                tris[tri_count * 3 + 1] = b;
                tris[tri_count * 3 + 2] = c;
                tri_count++;
            }
        } else if (mode == 6) {
            uint32_t a = gltf_read_index(index_view, 0);
            for (int32_t i = 1; i + 1 < count; i++) {
                uint32_t b = gltf_read_index(index_view, i);
                uint32_t c = gltf_read_index(index_view, i + 1);
                if (a >= (uint32_t)vertex_count || b >= (uint32_t)vertex_count ||
                    c >= (uint32_t)vertex_count || a == b || b == c || a == c)
                    continue;
                tris[tri_count * 3 + 0] = a;
                tris[tri_count * 3 + 1] = b;
                tris[tri_count * 3 + 2] = c;
                tri_count++;
            }
        }
        if (tri_count == 0) {
            rt_asset_error_add_import_stat(RT_ASSET_IMPORT_STAT_SKIPPED_PRIMITIVES, 1);
            rt_asset_error_add_warning(
                "glTF primitive skipped: its indices form no valid triangles");
            free(raw);
            free(tris);
            return;
        }
    }

    /* Greedy triangle bucketing: keep adding triangles while the bucket's bone
     * set stays within the draw palette. */
    bucket_of_tri = (int32_t *)malloc((size_t)tri_count * sizeof(int32_t));
    joint_bucket = (int32_t *)malloc((size_t)VGFX3D_MAX_SKELETON_BONES * sizeof(int32_t));
    joint_local = (int32_t *)malloc((size_t)VGFX3D_MAX_SKELETON_BONES * sizeof(int32_t));
    vertex_local = (int32_t *)malloc((size_t)vertex_count * sizeof(int32_t));
    if (!bucket_of_tri || !joint_bucket || !joint_local || !vertex_local)
        goto fail;
    for (int32_t j = 0; j < VGFX3D_MAX_SKELETON_BONES; j++)
        joint_bucket[j] = -1;
    {
        int32_t bucket_joint_count = 0;
        bucket_count = 1;
        for (int32_t t = 0; t < tri_count; t++) {
            int32_t fresh = 0;
            int32_t tri_joints[12];
            int32_t tri_joint_count = 0;
            for (int k = 0; k < 3; k++) {
                const gltf_mesh_import_vertex_t *v = &raw[tris[t * 3 + k]];
                for (int j = 0; j < 4; j++) {
                    if (v->weights[j] > 1e-8f &&
                        v->joints[j] < (uint32_t)VGFX3D_MAX_SKELETON_BONES) {
                        int32_t joint = (int32_t)v->joints[j];
                        int seen = 0;
                        for (int32_t q = 0; q < tri_joint_count; q++) {
                            if (tri_joints[q] == joint) {
                                seen = 1;
                                break;
                            }
                        }
                        if (!seen)
                            tri_joints[tri_joint_count++] = joint;
                    }
                }
            }
            for (int32_t q = 0; q < tri_joint_count; q++) {
                if (joint_bucket[tri_joints[q]] != bucket_count - 1)
                    fresh++;
            }
            if (bucket_joint_count + fresh > VGFX3D_MAX_BONES) {
                bucket_count++;
                bucket_joint_count = 0;
            }
            for (int32_t q = 0; q < tri_joint_count; q++) {
                if (joint_bucket[tri_joints[q]] != bucket_count - 1) {
                    joint_bucket[tri_joints[q]] = bucket_count - 1;
                    bucket_joint_count++;
                }
            }
            bucket_of_tri[t] = bucket_count - 1;
        }
    }

    if (!gltf_mesh_grow_output_tables(ctx, bucket_count))
        goto fail;

    /* Build one sub-mesh per bucket. */
    for (int32_t bucket = 0; bucket < bucket_count; bucket++) {
        rt_mesh3d *mesh = (rt_mesh3d *)rt_mesh3d_new();
        int32_t local_joint_count = 0;
        int32_t local_vertex_count = 0;
        int emitted = 0;
        if (!mesh)
            goto fail;
        for (int32_t j = 0; j < VGFX3D_MAX_SKELETON_BONES; j++)
            joint_local[j] = -1;
        for (int32_t vi = 0; vi < vertex_count; vi++)
            vertex_local[vi] = -1;
        rt_mesh3d_begin_geometry_batch(mesh);
        rt_mesh3d_reserve(mesh, vertex_count, tri_count);
        if (mesh->build_failed) {
            gltf_release_local(mesh);
            goto fail;
        }
        for (int32_t t = 0; t < tri_count; t++) {
            uint32_t local_idx[3];
            if (bucket_of_tri[t] != bucket)
                continue;
            for (int k = 0; k < 3; k++) {
                uint32_t src_vi = tris[t * 3 + k];
                if (vertex_local[src_vi] < 0) {
                    const gltf_mesh_import_vertex_t *v = &raw[src_vi];
                    int64_t local_joints[4] = {0, 0, 0, 0};
                    double local_weights[4] = {0.0, 0.0, 0.0, 0.0};
                    if (!gltf_mesh_append_import_vertex(
                            mesh, v->pos, v->nrm, v->uv, v->uv1, v->color, v->tangent)) {
                        gltf_release_local(mesh);
                        goto fail;
                    }
                    for (int j = 0; j < 4; j++) {
                        int32_t joint;
                        if (v->weights[j] <= 1e-8f ||
                            v->joints[j] >= (uint32_t)VGFX3D_MAX_SKELETON_BONES)
                            continue;
                        joint = (int32_t)v->joints[j];
                        if (joint_local[joint] < 0)
                            joint_local[joint] = local_joint_count++;
                        local_joints[j] = joint_local[joint];
                        local_weights[j] = v->weights[j];
                    }
                    rt_mesh3d_set_bone_weights(mesh,
                                               local_vertex_count,
                                               local_joints[0],
                                               local_weights[0],
                                               local_joints[1],
                                               local_weights[1],
                                               local_joints[2],
                                               local_weights[2],
                                               local_joints[3],
                                               local_weights[3]);
                    vertex_local[src_vi] = local_vertex_count++;
                }
                local_idx[k] = (uint32_t)vertex_local[src_vi];
            }
            emitted += gltf_emit_triangle(
                mesh, (uint32_t)local_vertex_count, local_idx[0], local_idx[1], local_idx[2]);
            if (mesh->build_failed) {
                gltf_release_local(mesh);
                goto fail;
            }
        }
        rt_mesh3d_end_geometry_batch(mesh);
        if (!emitted || local_joint_count <= 0) {
            gltf_release_local(mesh);
            continue;
        }
        /* Palette slot -> skeleton bone map. */
        mesh->bone_map = (int32_t *)malloc((size_t)local_joint_count * sizeof(int32_t));
        if (!mesh->bone_map) {
            gltf_release_local(mesh);
            goto fail;
        }
        for (int32_t j = 0; j < VGFX3D_MAX_SKELETON_BONES; j++) {
            if (joint_local[j] >= 0)
                mesh->bone_map[joint_local[j]] = j;
        }
        mesh->bone_count = local_joint_count;
        if ((!views->has_normals || invalid_authored_normals) && mesh->vertex_count > 0)
            rt_mesh3d_recalc_normals(mesh);
        if ((!views->has_tangents || invalid_authored_tangents) && views->has_uv0 &&
            material_idx >= 0 && material_idx < ctx->asset->material_count) {
            rt_material3d *tangent_material = (rt_material3d *)prim_material;
            if (rt_gltf_active_load_options()->force_tangents ||
                (tangent_material && tangent_material->normal_map))
                rt_mesh3d_calc_tangents(mesh);
        }
        ctx->asset->meshes[*mesh_idx_io] = mesh;
        (*mesh_idx_io)++;
        ctx->asset->mesh_count = *mesh_idx_io;
        if (ctx->primitive_materials)
            ctx->primitive_materials[*mesh_idx_io - 1] = prim_material;
        gltf_mesh_store_primitive_variant_mapping(ctx, prim, *mesh_idx_io - 1);
        emitted_meshes++;
    }

    gltf_emit_skin_import_warnings(mesh_json_index, primitive_index, &skin_stats);
    if (jarr_len(jarr(prim, "targets")) > 0)
        rt_asset_error_add_warningf(
            "glTF mesh %d primitive %d: morph targets dropped (primitive was partitioned "
            "across bone palettes)",
            mesh_json_index,
            primitive_index);
    if (emitted_meshes > 0)
        rt_asset_error_add_warningf(
            "glTF mesh %d primitive %d: skin exceeds %d bones; partitioned into %d "
            "sub-meshes",
            mesh_json_index,
            primitive_index,
            (int)VGFX3D_MAX_BONES,
            emitted_meshes);
    free(raw);
    free(tris);
    free(bucket_of_tri);
    free(joint_bucket);
    free(joint_local);
    free(vertex_local);
    return;
fail:
    free(raw);
    free(tris);
    free(bucket_of_tri);
    free(joint_bucket);
    free(joint_local);
    free(vertex_local);
    if (ctx->load_failed_io)
        *ctx->load_failed_io = 1;
}

/// @brief Import one glTF primitive into one or more runtime mesh slots.
/// @details Selects the Draco, preloaded, partitioned-skin, or conventional accessor
///          path; validates required UV streams; records materials and variant
///          mappings; and advances the output slot only for emitted meshes.
/// @param[in,out] ctx Shared mesh-load state and output tables.
/// @param[in] mesh_json Parsed parent mesh object.
/// @param[in] prim Parsed primitive object.
/// @param mesh_json_index Zero-based parent mesh index used for preloads and diagnostics.
/// @param primitive_index Zero-based primitive index used for preloads and diagnostics.
/// @param[in,out] mesh_idx_io Next free asset mesh slot, advanced for emitted meshes.
static void gltf_mesh_import_primitive(gltf_mesh_load_context_t *ctx,
                                       void *mesh_json,
                                       void *prim,
                                       int mesh_json_index,
                                       int primitive_index,
                                       int *mesh_idx_io) {
    void *attrs = jget(prim, "attributes");
    int64_t material_idx = jint(prim, "material", -1);
    int64_t mode = jint(prim, "mode", 4);
    void *prim_material = NULL;
    void *prim_extensions = jget(prim, "extensions");
    void *draco_ext = prim_extensions ? jget(prim_extensions, "KHR_draco_mesh_compression") : NULL;
    gltf_mesh_primitive_views_t views;
    void *mesh;
    int primitive_failed = 0;
    int invalid_authored_normals = 0;
    int invalid_authored_tangents = 0;
    gltf_skin_import_stats_t skin_stats = {0u, 0u};
    int preloaded_result;
    if (!attrs)
        return;
    prim_material = gltf_mesh_resolve_primitive_material(
        ctx->asset, material_idx, ctx->material_capacity, ctx->default_material_io);
    if (draco_ext) {
        gltf_draco_import_primitive(ctx,
                                    mesh_json,
                                    prim,
                                    draco_ext,
                                    mesh_json_index,
                                    primitive_index,
                                    prim_material,
                                    mesh_idx_io);
        return;
    }
    if (!(rt_gltf_active_load_options()->eight_bone_influences && jget(attrs, "JOINTS_1"))) {
        preloaded_result = gltf_mesh_take_preloaded_primitive(ctx->asset,
                                                              ctx->preload_bundle,
                                                              mesh_json_index,
                                                              primitive_index,
                                                              prim,
                                                              material_idx,
                                                              ctx->material_infos,
                                                              prim_material,
                                                              ctx->primitive_materials,
                                                              ctx->load_failed_io,
                                                              mesh_idx_io);
        if (preloaded_result > 0) {
            gltf_mesh_store_primitive_variant_mapping(ctx, prim, *mesh_idx_io - 1);
            return;
        }
        if (preloaded_result < 0)
            return;
    }
    if (!gltf_mesh_read_primitive_views(ctx->root,
                                        ctx->buffers,
                                        ctx->buf_count,
                                        attrs,
                                        prim,
                                        mode,
                                        &views,
                                        ctx->load_failed_io))
        return;
    if (!gltf_mesh_validate_primitive_uv_streams(ctx->asset,
                                                 ctx->material_infos,
                                                 prim,
                                                 material_idx,
                                                 views.has_uv0,
                                                 views.has_uv1,
                                                 mesh_json_index,
                                                 primitive_index,
                                                 ctx->load_failed_io))
        return;

    if ((views.has_skin0 || views.has_skin1) &&
        gltf_mesh_primitive_needs_partition(ctx->root, &views)) {
        gltf_mesh_import_partitioned_primitive(ctx,
                                               prim,
                                               &views,
                                               mesh_json_index,
                                               primitive_index,
                                               mode,
                                               material_idx,
                                               prim_material,
                                               mesh_idx_io);
        return;
    }

    mesh = rt_mesh3d_new();
    if (!mesh)
        return;
    rt_mesh3d_begin_geometry_batch((rt_mesh3d *)mesh);
    rt_mesh3d_reserve(mesh, views.pos.count, views.triangle_estimate);
    if (((rt_mesh3d *)mesh)->build_failed) {
        gltf_release_local(mesh);
        if (ctx->load_failed_io)
            *ctx->load_failed_io = 1;
        return;
    }
    if (rt_gltf_active_load_options()->eight_bone_influences && views.has_skin1 &&
        views.pos.count > 0) {
        ((rt_mesh3d *)mesh)->extra_influences = (vgfx3d_extra_influences_t *)calloc(
            (size_t)views.pos.count, sizeof(vgfx3d_extra_influences_t));
        if (!((rt_mesh3d *)mesh)->extra_influences) {
            gltf_release_local(mesh);
            if (ctx->load_failed_io)
                *ctx->load_failed_io = 1;
            return;
        }
    }

    primitive_failed = !gltf_mesh_import_vertices((rt_mesh3d *)mesh,
                                                  &views,
                                                  &invalid_authored_normals,
                                                  &invalid_authored_tangents,
                                                  &skin_stats);
    if (primitive_failed) {
        rt_mesh3d_end_geometry_batch((rt_mesh3d *)mesh);
        gltf_release_local(mesh);
        if (ctx->load_failed_io)
            *ctx->load_failed_io = 1;
        return;
    }

    if (!gltf_mesh_finalize_imported_primitive(ctx->asset,
                                               ctx->root,
                                               ctx->buffers,
                                               ctx->buf_count,
                                               mesh_json,
                                               prim,
                                               mesh,
                                               mode,
                                               &views,
                                               material_idx,
                                               prim_material,
                                               invalid_authored_normals,
                                               invalid_authored_tangents,
                                               ctx->load_failed_io)) {
        gltf_release_local(mesh);
        return;
    }

    gltf_emit_skin_import_warnings(mesh_json_index, primitive_index, &skin_stats);
    ctx->asset->meshes[*mesh_idx_io] = mesh;
    (*mesh_idx_io)++;
    ctx->asset->mesh_count = *mesh_idx_io;
    if (ctx->primitive_materials)
        ctx->primitive_materials[*mesh_idx_io - 1] = prim_material;
    gltf_mesh_store_primitive_variant_mapping(ctx, prim, *mesh_idx_io - 1);
}

/// @brief Import every primitive belonging to one parsed glTF mesh object.
/// @details Records the contiguous output-mesh range produced for the source mesh;
///          a single source primitive may contribute multiple slots when skin
///          partitioning is required.
/// @param[in,out] ctx Shared mesh-load state and output tables.
/// @param[in] meshes_arr Parsed glTF `meshes` array.
/// @param mesh_json_index Zero-based mesh object to import.
/// @param[out] mesh_prim_start Optional per-source-mesh first-slot table.
/// @param[out] mesh_prim_count Optional per-source-mesh emitted-count table.
/// @param[in,out] mesh_idx_io Next free output slot, advanced by imported primitives.
static void gltf_mesh_import_mesh_primitives(gltf_mesh_load_context_t *ctx,
                                             void *meshes_arr,
                                             int mesh_json_index,
                                             int *mesh_prim_start,
                                             int *mesh_prim_count,
                                             int *mesh_idx_io) {
    void *mesh_json = rt_seq_get(meshes_arr, (int64_t)mesh_json_index);
    void *prims = jarr(mesh_json, "primitives");
    int prim_count = (int)jarr_len(prims);
    int mesh_start = *mesh_idx_io;
    if (mesh_prim_start)
        mesh_prim_start[mesh_json_index] = mesh_start;
    if (mesh_prim_count)
        mesh_prim_count[mesh_json_index] = 0;
    for (int pi = 0; pi < prim_count; pi++)
        gltf_mesh_import_primitive(
            ctx, mesh_json, rt_seq_get(prims, (int64_t)pi), mesh_json_index, pi, mesh_idx_io);
    if (mesh_prim_count)
        mesh_prim_count[mesh_json_index] = *mesh_idx_io - mesh_start;
}

/// @brief Element stride of a decoded Draco attribute's output buffer.
/// @details Every decoder allocates num_values*components elements EXCEPT the
///          octahedral normal transform, which always emits 3 components. Parsing
///          rejects NORMALS attributes whose declared components != 3, so this
///          normally equals att->components; the explicit branch keeps the reader
///          memory-safe even if that invariant is ever relaxed.
/// @param[in] att Decoded Draco attribute descriptor.
/// @return Number of output scalar elements stored for each attribute value.
static size_t gltf_draco_attribute_out_stride(const draco_attribute *att) {
    return (att->decoder_type == DRACO_SEQ_NORMALS) ? 3u : (size_t)att->components;
}

/// @brief Read one float component from a decoded Draco attribute (handles both the
///        dequantized float and integer storage forms; integers are normalized when
///        the attribute was declared normalized).
/// @param[in] att Decoded attribute containing floating-point or integer output.
/// @param value Zero-based decoded attribute value index.
/// @param component Zero-based component within the selected value.
/// @return Component converted to floating point, including integer normalization
///         when requested; zero when the attribute has no decoded storage.
static float gltf_draco_attribute_float(const draco_attribute *att, uint32_t value, int component) {
    size_t index = (size_t)value * gltf_draco_attribute_out_stride(att) + (size_t)component;
    if (att->out_floats)
        return att->out_floats[index];
    if (!att->out_ints)
        return 0.0f;
    if (att->normalized) {
        /* Draco data types: 1 INT8, 2 UINT8, 3 INT16, 4 UINT16. */
        if (att->data_type == 2)
            return (float)att->out_ints[index] / 255.0f;
        if (att->data_type == 4)
            return (float)att->out_ints[index] / 65535.0f;
        if (att->data_type == 1)
            return (float)att->out_ints[index] / 127.0f;
        if (att->data_type == 3)
            return (float)att->out_ints[index] / 32767.0f;
    }
    return (float)att->out_ints[index];
}

/// @brief Build a Mesh3D from a KHR_draco_mesh_compression primitive payload.
/// @details Decodes the extension's bufferView through the Draco decoder (sequential
///          and edgebreaker encodings), then maps the extension's attribute ids onto
///          the decoded attributes (POSITION is required; NORMAL/TEXCOORD_0/
///          TEXCOORD_1/COLOR_0/JOINTS_0/WEIGHTS_0 are optional). Unsupported
///          encodings (point clouds) and corrupt payloads set load_failed with a
///          named diagnostic.
/// @param[in,out] ctx Shared mesh-load state, source buffers, and output tables.
/// @param[in] mesh_json Parsed parent mesh object used for morph-target import.
/// @param[in] prim Parsed primitive object.
/// @param[in] draco_ext Parsed `KHR_draco_mesh_compression` extension object.
/// @param mesh_json_index Zero-based parent mesh index used in diagnostics.
/// @param primitive_index Zero-based primitive index used in diagnostics.
/// @param[in] prim_material Resolved runtime material for the decoded mesh.
/// @param[in,out] mesh_idx_io Next free asset mesh slot, advanced on success.
static void gltf_draco_import_primitive(gltf_mesh_load_context_t *ctx,
                                        void *mesh_json,
                                        void *prim,
                                        void *draco_ext,
                                        int mesh_json_index,
                                        int primitive_index,
                                        void *prim_material,
                                        int *mesh_idx_io) {
    int64_t view_idx = jint(draco_ext, "bufferView", -1);
    void *ext_attrs = jget(draco_ext, "attributes");
    size_t payload_len = 0;
    const uint8_t *payload =
        gltf_get_buffer_view_data(ctx->root, view_idx, ctx->buffers, ctx->buf_count, &payload_len);
    draco_mesh draco;
    int unsupported = 0;
    const draco_attribute *pos_att;
    const draco_attribute *norm_att;
    const draco_attribute *uv0_att;
    const draco_attribute *uv1_att;
    const draco_attribute *color_att;
    const draco_attribute *joints_att;
    const draco_attribute *weights_att;
    gltf_skin_import_stats_t skin_stats = {0u, 0u};
    void *mesh;
    if (!payload || payload_len == 0 || !ext_attrs) {
        if (ctx->load_failed_io)
            *ctx->load_failed_io = 1;
        return;
    }
    if (!draco_decode_mesh(payload, payload_len, &draco, &unsupported)) {
        if (unsupported)
            rt_asset_error_setf(RT_ASSET_ERROR_UNSUPPORTED,
                                "GLTF.Load: mesh %d primitive %d uses an unsupported Draco "
                                "encoding (point cloud)",
                                mesh_json_index,
                                primitive_index);
        else
            rt_asset_error_setf(RT_ASSET_ERROR_CORRUPT,
                                "GLTF.Load: mesh %d primitive %d has corrupt "
                                "KHR_draco_mesh_compression data",
                                mesh_json_index,
                                primitive_index);
        if (ctx->load_failed_io)
            *ctx->load_failed_io = 1;
        return;
    }
    pos_att = draco_find_attribute(&draco, jint(ext_attrs, "POSITION", -1));
    norm_att = draco_find_attribute(&draco, jint(ext_attrs, "NORMAL", -1));
    uv0_att = draco_find_attribute(&draco, jint(ext_attrs, "TEXCOORD_0", -1));
    uv1_att = draco_find_attribute(&draco, jint(ext_attrs, "TEXCOORD_1", -1));
    color_att = draco_find_attribute(&draco, jint(ext_attrs, "COLOR_0", -1));
    joints_att = draco_find_attribute(&draco, jint(ext_attrs, "JOINTS_0", -1));
    weights_att = draco_find_attribute(&draco, jint(ext_attrs, "WEIGHTS_0", -1));
    if (!pos_att || pos_att->components < 3 || (!pos_att->out_floats && !pos_att->out_ints)) {
        draco_mesh_free(&draco);
        if (ctx->load_failed_io)
            *ctx->load_failed_io = 1;
        return;
    }
    if (norm_att && norm_att->components < 3)
        norm_att = NULL;
    if (uv0_att && uv0_att->components < 2)
        uv0_att = NULL;
    if (uv1_att && uv1_att->components < 2)
        uv1_att = NULL;
    if (joints_att && (!weights_att || joints_att->components < 4 || weights_att->components < 4 ||
                       !joints_att->out_ints)) {
        joints_att = NULL;
        weights_att = NULL;
    }
    if (!gltf_mesh_validate_primitive_uv_streams(ctx->asset,
                                                 ctx->material_infos,
                                                 prim,
                                                 jint(prim, "material", -1),
                                                 uv0_att != NULL,
                                                 uv1_att != NULL,
                                                 mesh_json_index,
                                                 primitive_index,
                                                 ctx->load_failed_io)) {
        draco_mesh_free(&draco);
        return;
    }

    mesh = rt_mesh3d_new();
    if (!mesh) {
        draco_mesh_free(&draco);
        return;
    }
    rt_mesh3d_begin_geometry_batch((rt_mesh3d *)mesh);
    rt_mesh3d_reserve(mesh, (int64_t)draco.num_points, (int64_t)draco.num_faces);
    if (((rt_mesh3d *)mesh)->build_failed)
        goto fail;
    for (uint32_t i = 0; i < draco.num_points; i++) {
        float pos[3];
        float nrm[3] = {0.0f, 0.0f, 0.0f};
        float uv[2] = {0.0f, 0.0f};
        float uv1[2];
        float color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        float tangent[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        for (int c = 0; c < 3; c++)
            pos[c] = gltf_draco_attribute_float(pos_att, i, c);
        if (norm_att)
            for (int c = 0; c < 3; c++)
                nrm[c] = gltf_draco_attribute_float(norm_att, i, c);
        if (uv0_att)
            for (int c = 0; c < 2; c++)
                uv[c] = gltf_draco_attribute_float(uv0_att, i, c);
        if (uv1_att) {
            for (int c = 0; c < 2; c++)
                uv1[c] = gltf_draco_attribute_float(uv1_att, i, c);
        } else {
            uv1[0] = uv[0];
            uv1[1] = uv[1];
        }
        if (color_att) {
            for (int c = 0; c < color_att->components && c < 4; c++)
                color[c] = gltf_draco_attribute_float(color_att, i, c);
        }
        if (!gltf_f32_array_is_finite(pos, 3) || !gltf_f32_array_is_finite(nrm, 3) ||
            !gltf_f32_array_is_finite(uv, 2) || !gltf_f32_array_is_finite(color, 4))
            goto fail;
        if (norm_att && !gltf_normalize_vec3f_in_place(nrm)) {
            nrm[0] = 0.0f;
            nrm[1] = 0.0f;
            nrm[2] = 0.0f;
        }
        if (!gltf_mesh_append_import_vertex((rt_mesh3d *)mesh, pos, nrm, uv, uv1, color, tangent))
            goto fail;
        if (joints_att) {
            uint32_t joints[4];
            float weights[4];
            int out_of_range = 0;
            for (int c = 0; c < 4; c++) {
                int32_t j = joints_att->out_ints[(size_t)i * joints_att->components + c];
                joints[c] = j < 0 ? 0u : (uint32_t)j;
                weights[c] = gltf_draco_attribute_float(weights_att, i, c);
                if (!isfinite(weights[c]) || weights[c] < 0.0f)
                    weights[c] = 0.0f;
                if (joints[c] >= VGFX3D_MAX_BONES) {
                    if (weights[c] > 1e-8f)
                        out_of_range = 1;
                    joints[c] = 0u;
                    weights[c] = 0.0f;
                }
            }
            if (out_of_range)
                skin_stats.out_of_range_vertices++;
            gltf_normalize_joint_influences(weights);
            rt_mesh3d_set_bone_weights(mesh,
                                       (int32_t)i,
                                       (int64_t)joints[0],
                                       weights[0],
                                       (int64_t)joints[1],
                                       weights[1],
                                       (int64_t)joints[2],
                                       weights[2],
                                       (int64_t)joints[3],
                                       weights[3]);
            for (int c = 0; c < 4; c++) {
                if (weights[c] > 0.0001f &&
                    (int32_t)(joints[c] + 1u) > ((rt_mesh3d *)mesh)->bone_count)
                    ((rt_mesh3d *)mesh)->bone_count = (int32_t)(joints[c] + 1u);
            }
        }
    }
    {
        int emitted = 0;
        for (uint32_t f = 0; f < draco.num_faces; f++) {
            emitted += gltf_emit_triangle(mesh,
                                          draco.num_points,
                                          draco.face_to_vertex[f * 3u],
                                          draco.face_to_vertex[f * 3u + 1u],
                                          draco.face_to_vertex[f * 3u + 2u]);
            if (((rt_mesh3d *)mesh)->build_failed)
                goto fail;
        }
        rt_mesh3d_end_geometry_batch((rt_mesh3d *)mesh);
        if (!emitted) {
            rt_asset_error_add_import_stat(RT_ASSET_IMPORT_STAT_SKIPPED_PRIMITIVES, 1);
            rt_asset_error_add_warning(
                "glTF primitive skipped: its Draco indices form no valid triangles");
            gltf_release_local(mesh);
            draco_mesh_free(&draco);
            return;
        }
    }
    if (!norm_att && ((rt_mesh3d *)mesh)->vertex_count > 0)
        rt_mesh3d_recalc_normals(mesh);
    if (uv0_att) {
        rt_material3d *tangent_material = (rt_material3d *)prim_material;
        if (rt_gltf_active_load_options()->force_tangents ||
            (tangent_material && tangent_material->normal_map))
            rt_mesh3d_calc_tangents(mesh);
    }
    gltf_import_primitive_morph_targets(
        ctx->root, ctx->buffers, ctx->buf_count, mesh_json, prim, mesh, (int32_t)draco.num_points);
    gltf_emit_skin_import_warnings(mesh_json_index, primitive_index, &skin_stats);
    draco_mesh_free(&draco);
    ctx->asset->meshes[*mesh_idx_io] = mesh;
    (*mesh_idx_io)++;
    ctx->asset->mesh_count = *mesh_idx_io;
    if (ctx->primitive_materials)
        ctx->primitive_materials[*mesh_idx_io - 1] = prim_material;
    gltf_mesh_store_primitive_variant_mapping(ctx, prim, *mesh_idx_io - 1);
    return;
fail:
    rt_mesh3d_end_geometry_batch((rt_mesh3d *)mesh);
    gltf_release_local(mesh);
    draco_mesh_free(&draco);
    if (ctx->load_failed_io)
        *ctx->load_failed_io = 1;
}

/// @brief Decode glTF meshes/primitives into Mesh3D objects on the asset.
///        Extracted post-asset phase of rt_gltf_load_impl. Malformed primitives set
///        load_failed_io so corrupt assets do not render as partial/empty scenes.
/// @note out_mesh_prim_start/out_mesh_prim_count map a glTF mesh index to its
///       owning asset->meshes range; the material and variant-mapping tables
///       parallel asset->meshes. All returned arrays are owned by caller cleanup.
/// @param[in,out] asset Asset receiving the decoded runtime meshes.
/// @param[in] root Parsed glTF document root.
/// @param[in] buffers Resolved glTF buffer table.
/// @param buf_count Number of entries in @p buffers.
/// @param[in,out] preload_bundle Optional bundle of asynchronously decoded primitives.
/// @param[in] material_infos Per-material UV and feature metadata.
/// @param[out] out_mesh_prim_start Receives the owned per-source-mesh first-slot table.
/// @param[out] out_mesh_prim_count Receives the owned per-source-mesh emitted-count table.
/// @param[out] out_primitive_materials Receives the owned per-output-mesh material table.
/// @param[out] out_primitive_variant_mappings Receives the owned per-output-mesh
///                                             variant-mapping pointer table.
/// @param[out] out_primitive_variant_mapping_count Receives the allocated capacity
///                                                  of the variant-mapping table.
/// @param[in,out] load_failed_io Optional flag raised when malformed data, overflow,
///                               or allocation failure prevents a complete import.
static void gltf_load_meshes(rt_gltf_asset *asset,
                             void *root,
                             gltf_buffer_t *buffers,
                             int buf_count,
                             rt_gltf_preload_bundle *preload_bundle,
                             const gltf_material_info_t *material_infos,
                             int **out_mesh_prim_start,
                             int **out_mesh_prim_count,
                             void ***out_primitive_materials,
                             int32_t ***out_primitive_variant_mappings,
                             int32_t *out_primitive_variant_mapping_count,
                             int *load_failed_io) {
    int *mesh_prim_start = NULL;
    int *mesh_prim_count = NULL;
    void **primitive_materials = NULL;
    int32_t **primitive_variant_mappings = NULL;
    int32_t primitive_variant_mapping_count = 0;
    void *default_material = NULL;
    int64_t gltf_material_count_raw = jarr_len(jarr(root, "materials"));
    int gltf_material_count =
        gltf_material_count_raw > INT32_MAX ? INT32_MAX : (int)gltf_material_count_raw;
    int material_capacity = 1;
    // Extract meshes
    void *meshes_arr = jarr(root, "meshes");
    int64_t mesh_json_count_raw = jarr_len(meshes_arr);
    int mesh_json_count = mesh_json_count_raw > INT32_MAX ? INT32_MAX : (int)mesh_json_count_raw;
    if (gltf_material_count_raw > INT32_MAX - 1 || mesh_json_count_raw > INT32_MAX) {
        if (load_failed_io)
            *load_failed_io = 1;
        goto finish;
    }
    material_capacity = gltf_material_count > 0 ? gltf_material_count + 1 : 1;
    if (mesh_json_count > 0) {
        int total_prims = 0;
        if (!gltf_mesh_count_total_primitives(
                meshes_arr, mesh_json_count, &total_prims, load_failed_io))
            goto finish;

        if (total_prims > 0) {
            gltf_mesh_load_context_t ctx;
            if (!gltf_mesh_alloc_output_tables(asset,
                                               mesh_json_count,
                                               total_prims,
                                               &mesh_prim_start,
                                               &mesh_prim_count,
                                               &primitive_materials,
                                               &primitive_variant_mappings,
                                               load_failed_io))
                goto finish;
            ctx.asset = asset;
            ctx.material_infos = material_infos;
            ctx.root = root;
            ctx.buffers = buffers;
            ctx.buf_count = buf_count;
            ctx.preload_bundle = preload_bundle;
            ctx.material_capacity = material_capacity;
            ctx.primitive_materials = primitive_materials;
            ctx.primitive_variant_mappings = primitive_variant_mappings;
            ctx.default_material_io = &default_material;
            ctx.load_failed_io = load_failed_io;
            ctx.primitive_table_capacity = total_prims;
            int mesh_idx = 0;
            for (int mi = 0; mi < mesh_json_count && asset->meshes; mi++) {
                gltf_mesh_import_mesh_primitives(
                    &ctx, meshes_arr, mi, mesh_prim_start, mesh_prim_count, &mesh_idx);
            }
            /* Partitioned skins may have grown (reallocated) the side tables. */
            primitive_materials = ctx.primitive_materials;
            primitive_variant_mappings = ctx.primitive_variant_mappings;
            primitive_variant_mapping_count = ctx.primitive_table_capacity;
        }
    }
finish:
    *out_mesh_prim_start = mesh_prim_start;
    *out_mesh_prim_count = mesh_prim_count;
    *out_primitive_materials = primitive_materials;
    *out_primitive_variant_mappings = primitive_variant_mappings;
    *out_primitive_variant_mapping_count = primitive_variant_mapping_count;
}
