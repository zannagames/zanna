//===----------------------------------------------------------------------===//
// Part of the Zanna project, under the GNU GPL v3. See LICENSE.
// File: src/runtime/graphics/3d/assets/rt_fbx_loader_skeleton.inc  (#included from rt_fbx_loader.c;
// not a standalone translation unit) Purpose: FBX skeleton extraction
//===----------------------------------------------------------------------===//

/**
 * @file rt_fbx_loader_skeleton.inc
 * @brief Extracts FBX skeletal Model hierarchies and reconciles their bind poses.
 *
 * Skeleton Models are ordered parent-first, composed through the full FBX transform
 * stack, and converted into runtime-local bind matrices. Authoritative Cluster
 * `TransformLink` globals take precedence, followed by an evaluated constrained pose
 * and finally the Model hierarchy. The resulting Model-ID bindings are retained only
 * as load-local metadata for skinning and animation import.
 */

/*==========================================================================
 * Skeleton extraction
 *=========================================================================*/

/// @brief Find a Cluster bind matrix connected to a bone model.
/// @details Skinned FBX files often store the authoritative bone bind pose in the Cluster
///          `TransformLink` matrix instead of exactly matching the bone Model's current local
///          transform. This scans all Cluster deformers connected to @p model_id and returns the
///          first finite `TransformLink` matrix, with optional Z-up basis conversion applied.
///          `TransformAssociateModel` is accepted as a compatibility fallback for exporters that
///          use it for link-style matrices.
/// @param objects FBX `Objects` node.
/// @param ct Parsed connection table.
/// @param model_id Bone Model object ID.
/// @param z_up Non-zero when source coordinates are Z-up.
/// @param out Receives the row-major global bind matrix on success.
/// @return Non-zero when a cluster link matrix was found.
static int fbx_find_cluster_transform_link_for_model(
    fbx_node_t *objects, const fbx_conn_table_t *ct, int64_t model_id, int z_up, double out[16]) {
    if (!objects || !ct || model_id == 0 || !out)
        return 0;
    for (int32_t oi = 0; oi < objects->child_count; oi++) {
        fbx_node_t *cluster = &objects->children[oi];
        int64_t cluster_id;
        int connected = 0;
        if (strcmp(cluster->name, "Deformer") != 0 || cluster->prop_count < 3 ||
            strcmp(fbx_prop_str(cluster, 2), "Cluster") != 0)
            continue;
        cluster_id = fbx_prop_i64(cluster, 0);
        for (int32_t ci = fbx_connections_first_for_child(ct, cluster_id); ci >= 0;
             ci = fbx_connections_next_for_child(ct, ci)) {
            if (ct->entries[ci].parent_id == model_id) {
                connected = 1;
                break;
            }
        }
        if (!connected)
            continue;
        if (!fbx_read_child_matrix16(cluster, "TransformLink", out) &&
            !fbx_read_child_matrix16(cluster, "TransformAssociateModel", out))
            continue;
        if (z_up)
            fbx_mat4_apply_zup(out);
        return 1;
    }
    return 0;
}

/// @brief Load-local transform and hierarchy metadata for one skeletal FBX Model.
typedef struct {
    /// Numeric FBX Model identifier.
    int64_t id;
    /// Borrowed source Model node.
    fbx_node_t *source;
    /// Parsed full FBX transform-stack components.
    fbx_transform_components_t transform;
    /// Composed Model-local bind matrix after coordinate conversion.
    double local_model_bind[16];
    /// Authoritative global Cluster link bind matrix when available.
    double cluster_global_bind[16];
    /// Non-zero when @ref cluster_global_bind contains a valid matrix.
    int has_cluster_global_bind;
    /// Numeric parent Model identifier.
    int64_t parent_id;
    /// Runtime skeleton index assigned after topological ordering.
    int32_t bone_index; /* assigned after topological sort */
} fbx_skel_bone_info_t;

/// @brief Order skeleton bones parent-first (roots, then children whose parent is already
///        placed).
/// @param bones Unordered bone metadata array.
/// @param bone_count Number of entries in @p bones.
/// @param[out] order Receives source-array indices in parent-first order.
/// @param[in,out] placed Scratch flags, marked for every emitted source index.
/// @return Number of bones successfully placed. A value below @p bone_count indicates
///         a cycle or parent chain that cannot be resolved within the collected set.
static int32_t fbx_skeleton_topo_order(const fbx_skel_bone_info_t *bones,
                                       int32_t bone_count,
                                       int32_t *order,
                                       int8_t *placed) {
    int32_t placed_count = 0;
    /* Place roots first */
    for (int32_t i = 0; i < bone_count; i++) {
        int is_root = 1;
        for (int32_t j = 0; j < bone_count; j++)
            if (bones[i].parent_id == bones[j].id) {
                is_root = 0;
                break;
            }
        if (is_root) {
            order[placed_count++] = i;
            placed[i] = 1;
        }
    }

    /* Place children */
    for (int32_t pass = 0; pass < bone_count && placed_count < bone_count; pass++) {
        for (int32_t i = 0; i < bone_count; i++) {
            if (placed[i])
                continue;
            for (int32_t j = 0; j < placed_count; j++) {
                if (bones[i].parent_id == bones[order[j]].id) {
                    order[placed_count++] = i;
                    placed[i] = 1;
                    break;
                }
            }
        }
    }
    return placed_count;
}

/// @brief Build a `rt_skeleton3d_t` from the FBX `Model` nodes of type LimbNode/Limb/Root.
///
/// Walks the connection table to determine each bone's parent
/// (translating bone-IDs to in-skeleton indices), composes the full FBX
/// transform stack for each bone, then reconciles that local hierarchy with
/// Cluster `TransformLink` global bind matrices when present. `z_up` triggers
/// the same basis conversion as the geometry pass, but at matrix level so
/// rotations, pivots, and bind poses stay coherent.
/// @param root Parsed FBX document root.
/// @param ct Validated object connection table.
/// @param constraint_pose Optional evaluated static constraint pose used when no
///                        Cluster link bind is available.
/// @param z_up Non-zero to convert all bind matrices into runtime Y-up space.
/// @param[out] out_bindings Optional destination for the newly allocated
///                          Model-ID-to-bone-index binding array.
/// @param[out] out_binding_count Optional destination for the binding count.
/// @return Newly allocated runtime Skeleton3D on success, or NULL when no skeletal
///         Models exist, hierarchy ordering fails, or allocation/bone publication fails.
static void *fbx_extract_skeleton(fbx_node_t *root,
                                  const fbx_conn_table_t *ct,
                                  const fbx_constraint_pose_t *constraint_pose,
                                  int z_up,
                                  fbx_bone_binding_t **out_bindings,
                                  int32_t *out_binding_count) {
    fbx_bone_binding_t *bindings = NULL;
    int32_t binding_count = 0;
    if (out_bindings)
        *out_bindings = NULL;
    if (out_binding_count)
        *out_binding_count = 0;
    fbx_node_t *objects = fbx_find_child(root, "Objects");
    if (!objects)
        return NULL;

    /* Collect Model nodes that are skeleton limbs */

    fbx_skel_bone_info_t *bones = NULL;
    int32_t bone_count = 0;
    int32_t bone_cap = 0;

    for (int32_t i = 0; i < objects->child_count; i++) {
        fbx_node_t *obj = &objects->children[i];
        if (strcmp(obj->name, "Model") != 0 || obj->prop_count < 3)
            continue;
        const char *type_str = fbx_prop_str(obj, 2);
        if (strcmp(type_str, "LimbNode") != 0 && strcmp(type_str, "Limb") != 0 &&
            strcmp(type_str, "Root") != 0)
            continue;
        if (bone_count >= FBX_MAX_SKELETON_BONES)
            continue;

        if (bone_count >= bone_cap) {
            if (bone_cap < 0 || bone_cap > INT32_MAX / 2 ||
                (size_t)(bone_cap == 0 ? 32 : bone_cap * 2) >
                    SIZE_MAX / sizeof(fbx_skel_bone_info_t))
                break;
            int32_t new_cap = bone_cap == 0 ? 32 : bone_cap * 2;
            fbx_skel_bone_info_t *nb = (fbx_skel_bone_info_t *)realloc(
                bones, (size_t)new_cap * sizeof(fbx_skel_bone_info_t));
            if (!nb)
                break;
            bones = nb;
            bone_cap = new_cap;
        }

        static const fbx_skel_bone_info_t zero_bone_info = {0};
        fbx_skel_bone_info_t *bi = &bones[bone_count++];
        *bi = zero_bone_info;
        bi->id = fbx_prop_i64(obj, 0);
        bi->source = obj;
        fbx_read_model_transform_components(obj, &bi->transform);
        fbx_build_model_transform_matrix(&bi->transform, bi->local_model_bind);
        if (z_up)
            fbx_mat4_apply_zup(bi->local_model_bind);
        bi->has_cluster_global_bind = fbx_find_cluster_transform_link_for_model(
            objects, ct, bi->id, z_up, bi->cluster_global_bind);

        bi->parent_id = fbx_find_model_parent(objects, ct, bi->id);
    }

    if (bone_count == 0) {
        free(bones);
        return NULL;
    }

    /* Build skeleton in topological order */
    void *skel = rt_skeleton3d_new();
    if (!skel) {
        free(bones);
        return NULL;
    }

    /* Assign bone indices: process bones in parent-first order */
    int32_t *order = (int32_t *)calloc((size_t)bone_count, sizeof(int32_t));
    int8_t *placed = (int8_t *)calloc((size_t)bone_count, sizeof(int8_t));
    double *global_binds = NULL;
    int32_t placed_count = 0;
    int failed = 0;
    if (!order || !placed) {
        free(order);
        free(placed);
        free(bones);
        fbx_release_ref(&skel);
        return NULL;
    }

    placed_count = fbx_skeleton_topo_order(bones, bone_count, order, placed);
    if (placed_count <= 0) {
        free(order);
        free(placed);
        free(bones);
        fbx_release_ref(&skel);
        return NULL;
    }
    if (out_bindings && out_binding_count) {
        bindings = (fbx_bone_binding_t *)calloc((size_t)placed_count, sizeof(*bindings));
        if (!bindings) {
            free(order);
            free(placed);
            free(bones);
            fbx_release_ref(&skel);
            return NULL;
        }
    }
    if (placed_count > 0) {
        if ((size_t)placed_count <= SIZE_MAX / (16u * sizeof(double)))
            global_binds = (double *)calloc((size_t)placed_count * 16u, sizeof(double));
        if (!global_binds) {
            free(bindings);
            free(order);
            free(placed);
            free(bones);
            fbx_release_ref(&skel);
            return NULL;
        }
    }

    /* Add bones to skeleton in topological order */
    for (int32_t i = 0; i < placed_count; i++) {
        fbx_skel_bone_info_t *bi = &bones[order[i]];
        int64_t parent_idx = -1;
        double global_bind[16];
        double local_bind[16];
        for (int32_t j = 0; j < i; j++) {
            if (bi->parent_id == bones[order[j]].id) {
                parent_idx = j;
                break;
            }
        }

        const double *constraint_global =
            fbx_constraint_static_global_for_id(constraint_pose, bi->id);
        if (bi->has_cluster_global_bind) {
            memcpy(global_bind, bi->cluster_global_bind, sizeof(global_bind));
        } else if (constraint_global) {
            memcpy(global_bind, constraint_global, sizeof(global_bind));
        } else if (parent_idx >= 0) {
            fbx_build_inherited_global_matrix(&global_binds[(size_t)parent_idx * 16u],
                                              &bones[order[parent_idx]].transform,
                                              bi->local_model_bind,
                                              &bi->transform,
                                              global_bind);
        } else {
            memcpy(global_bind, bi->local_model_bind, sizeof(global_bind));
        }
        memcpy(&global_binds[(size_t)i * 16u], global_bind, sizeof(global_bind));

        if (parent_idx >= 0) {
            double inv_parent[16];
            if (fbx_mat4_inverse_affine(&global_binds[(size_t)parent_idx * 16u], inv_parent))
                fbx_mat4_mul_local(inv_parent, global_bind, local_bind);
            else
                memcpy(local_bind, bi->local_model_bind, sizeof(local_bind));
        } else {
            memcpy(local_bind, global_bind, sizeof(local_bind));
        }

        void *bind_mat = fbx_mat4_to_rt(local_bind);
        rt_string bone_name = fbx_object_name_string(bi->source, 1);
        if (!bone_name)
            bone_name = rt_const_cstr("Bone");
        int64_t added = rt_skeleton3d_add_bone(skel, bone_name, parent_idx, bind_mat);
        rt_string_unref(bone_name);
        fbx_release_ref(&bind_mat);
        if (added < 0) {
            failed = 1;
            break;
        }
        bi->bone_index = (int32_t)added;
        if (bindings) {
            bindings[binding_count].model_id = bi->id;
            bindings[binding_count].bone_index = (int32_t)added;
            binding_count++;
        }
    }
    if (failed) {
        free(bindings);
        free(global_binds);
        free(order);
        free(placed);
        free(bones);
        fbx_release_ref(&skel);
        return NULL;
    }

    rt_skeleton3d_compute_inverse_bind(skel);

    free(global_binds);
    free(order);
    free(placed);
    free(bones);
    if (out_bindings)
        *out_bindings = bindings;
    if (out_binding_count)
        *out_binding_count = binding_count;
    return skel;
}
