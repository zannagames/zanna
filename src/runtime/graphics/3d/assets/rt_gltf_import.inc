//===----------------------------------------------------------------------===//
// Part of the Zanna project, under the GNU GPL v3. See LICENSE.
// File: src/runtime/graphics/3d/assets/rt_gltf_import.inc  (#included from rt_gltf.c; not a
// standalone translation unit) Purpose: glTF decoded-payload take, scene/camera/light/morph import,
// path/URI resolution
//===----------------------------------------------------------------------===//

/**
 * @file rt_gltf_import.inc
 * @brief Implements glTF scene-resource import, preload consumption, and dependency resolution.
 * @details Included by `rt_gltf.c`, this fragment converts staged mesh and morph payloads into
 *          runtime objects, sanitizes transforms, clones scene hierarchies, imports cameras and
 *          punctual lights, attaches morph targets, and resolves external buffer or image URIs
 *          through preload bundles, the asset manager, or bounded filesystem reads. Ownership
 *          transfers are explicit so partial failures can release all temporary state safely.
 */

/// @brief Unpack a primitive's preloaded "VGMT" morph-target POD blob and attach it to @p mesh_obj.
/// @details Looks up the morph dependency by mesh/primitive key, validates the header (magic,
///          version, matching vertex count) and every record/name/payload offset against the blob
///          length, then rebuilds an rt_morphtarget3d — adding each named shape with its weight and
///          per-vertex position/normal/tangent deltas (only the flagged channels). Missing morph
///          blobs are allowed; malformed present blobs fail the caller's asset load.
/// @param preload_bundle Bundle from which the primitive's morph dependency is consumed.
/// @param mesh_index Source glTF mesh index used to construct the dependency key.
/// @param primitive_index Primitive index within the source mesh.
/// @param mesh_obj Runtime mesh receiving the reconstructed morph target.
/// @param vertex_count Expected vertex count shared by the mesh and every morph channel.
/// @return 1 when the morph dependency is absent or fully attached; 0 when a present dependency is
///         corrupt or cannot be applied.
static int gltf_preload_take_decoded_morph(rt_gltf_preload_bundle *preload_bundle,
                                           int mesh_index,
                                           int primitive_index,
                                           void *mesh_obj,
                                           uint32_t vertex_count) {
    char key[96];
    uint8_t *blob;
    size_t blob_len = 0;
    uint32_t magic;
    uint32_t version;
    uint32_t pod_vertex_count;
    uint32_t shape_count;
    uint32_t record_bytes_u32;
    uint32_t names_bytes_u32;
    uint32_t payload_bytes_u32;
    size_t record_bytes;
    size_t names_bytes;
    size_t payload_bytes;
    size_t records_offset;
    size_t names_offset;
    size_t payload_offset;
    size_t required_len;
    size_t channel_bytes;
    void *morph = NULL;

    if (!preload_bundle || !mesh_obj || vertex_count == 0 || vertex_count > INT32_MAX)
        return 1;
    gltf_preload_morph_key(mesh_index, primitive_index, key, sizeof(key));
    blob = gltf_preload_bundle_take_dependency(
        preload_bundle, key, GLTF_PRELOAD_DEP_MORPH_POD, &blob_len);
    if (!blob)
        return 1;
    if (blob_len < GLTF_PRELOAD_MORPH_POD_HEADER_SIZE)
        goto malformed;
    magic = gltf_read_u32_le(blob + 0);
    version = gltf_read_u32_le(blob + 4);
    pod_vertex_count = gltf_read_u32_le(blob + 8);
    shape_count = gltf_read_u32_le(blob + 12);
    record_bytes_u32 = gltf_read_u32_le(blob + 16);
    names_bytes_u32 = gltf_read_u32_le(blob + 20);
    payload_bytes_u32 = gltf_read_u32_le(blob + 24);
    if (magic != GLTF_PRELOAD_MORPH_POD_MAGIC || version != GLTF_PRELOAD_MORPH_POD_VERSION ||
        pod_vertex_count != vertex_count || shape_count == 0 || shape_count > INT32_MAX)
        goto malformed;
    if (!gltf_checked_mul_size(
            (size_t)shape_count, GLTF_PRELOAD_MORPH_POD_RECORD_SIZE, &record_bytes) ||
        record_bytes != (size_t)record_bytes_u32)
        goto malformed;
    names_bytes = (size_t)names_bytes_u32;
    payload_bytes = (size_t)payload_bytes_u32;
    records_offset = GLTF_PRELOAD_MORPH_POD_HEADER_SIZE;
    if (!gltf_checked_add_size(records_offset, record_bytes, &names_offset) ||
        !gltf_checked_add_size(names_offset, names_bytes, &payload_offset) ||
        !gltf_checked_add_size(payload_offset, payload_bytes, &required_len) ||
        required_len > blob_len)
        goto malformed;
    if (!gltf_checked_mul_size((size_t)vertex_count, 3u * sizeof(float), &channel_bytes))
        goto malformed;

    morph = rt_morphtarget3d_new((int64_t)vertex_count);
    if (!morph)
        goto malformed;
    for (uint32_t si = 0; si < shape_count; si++) {
        const uint8_t *record =
            blob + records_offset + (size_t)si * GLTF_PRELOAD_MORPH_POD_RECORD_SIZE;
        uint32_t flags = gltf_read_u32_le(record + 0);
        uint32_t name_offset = gltf_read_u32_le(record + 4);
        uint32_t name_len = gltf_read_u32_le(record + 8);
        float weight = gltf_read_f32_le(record + 12);
        uint32_t pos_offset = gltf_read_u32_le(record + 16);
        uint32_t norm_offset = gltf_read_u32_le(record + 20);
        uint32_t tan_offset = gltf_read_u32_le(record + 24);
        const char *name;
        int64_t shape;
        if (name_len == 0 || (size_t)name_offset >= names_bytes ||
            (size_t)name_len > names_bytes - (size_t)name_offset)
            goto malformed;
        name = (const char *)(blob + names_offset + name_offset);
        if (name[name_len - 1u] != '\0')
            goto malformed;
        if ((flags & GLTF_PRELOAD_MORPH_POD_HAS_POSITIONS) != 0 &&
            ((size_t)pos_offset > payload_bytes ||
             channel_bytes > payload_bytes - (size_t)pos_offset))
            goto malformed;
        if ((flags & GLTF_PRELOAD_MORPH_POD_HAS_NORMALS) != 0 &&
            ((size_t)norm_offset > payload_bytes ||
             channel_bytes > payload_bytes - (size_t)norm_offset))
            goto malformed;
        if ((flags & GLTF_PRELOAD_MORPH_POD_HAS_TANGENTS) != 0 &&
            ((size_t)tan_offset > payload_bytes ||
             channel_bytes > payload_bytes - (size_t)tan_offset))
            goto malformed;
        {
            rt_string runtime_name = rt_const_cstr(name);
            shape = rt_morphtarget3d_add_shape(morph, runtime_name);
            rt_string_unref(runtime_name);
        }
        if (shape < 0)
            goto malformed;
        rt_morphtarget3d_set_weight(morph, shape, weight);
        for (uint32_t vi = 0; vi < vertex_count; vi++) {
            if ((flags & GLTF_PRELOAD_MORPH_POD_HAS_POSITIONS) != 0) {
                const uint8_t *src =
                    blob + payload_offset + (size_t)pos_offset + (size_t)vi * 3u * sizeof(float);
                rt_morphtarget3d_set_delta(morph,
                                           shape,
                                           vi,
                                           gltf_read_f32_le(src + 0),
                                           gltf_read_f32_le(src + 4),
                                           gltf_read_f32_le(src + 8));
            }
            if ((flags & GLTF_PRELOAD_MORPH_POD_HAS_NORMALS) != 0) {
                const uint8_t *src =
                    blob + payload_offset + (size_t)norm_offset + (size_t)vi * 3u * sizeof(float);
                rt_morphtarget3d_set_normal_delta(morph,
                                                  shape,
                                                  vi,
                                                  gltf_read_f32_le(src + 0),
                                                  gltf_read_f32_le(src + 4),
                                                  gltf_read_f32_le(src + 8));
            }
            if ((flags & GLTF_PRELOAD_MORPH_POD_HAS_TANGENTS) != 0) {
                const uint8_t *src =
                    blob + payload_offset + (size_t)tan_offset + (size_t)vi * 3u * sizeof(float);
                rt_morphtarget3d_set_tangent_delta(morph,
                                                   shape,
                                                   vi,
                                                   gltf_read_f32_le(src + 0),
                                                   gltf_read_f32_le(src + 4),
                                                   gltf_read_f32_le(src + 8));
            }
        }
    }
    if (rt_morphtarget3d_get_shape_count(morph) > 0)
        rt_mesh3d_set_morph_targets(mesh_obj, morph);

    gltf_release_local(morph);
    free(blob);
    return 1;

malformed:
    rt_asset_error_set_if_empty(RT_ASSET_ERROR_CORRUPT,
                                "GLTF.Load: malformed preloaded morph data");
    gltf_release_local(morph);
    free(blob);
    return 0;
}

/// @brief Unpack a primitive's preloaded "VGMP" mesh POD blob into a new Mesh3D.
/// @details Looks up the mesh dependency by key, validates the header and that the declared
///          vertex/index byte spans fit the blob, then installs the deserialized vertex and index
///          arrays directly into a fresh mesh's internals (a fast path that bypasses per-vertex
///          appends), attaches any morph targets, and returns the primitive's flags via @p
///          out_flags.
/// @param preload_bundle Bundle from which the primitive dependency is consumed.
/// @param mesh_index Source glTF mesh index used in the dependency key.
/// @param primitive_index Primitive index within the source mesh.
/// @param out_flags Optional destination for the validated packed-mesh feature flags.
/// @return New Mesh3D handle, or NULL if the blob is missing or malformed.
static void *gltf_preload_take_decoded_mesh(rt_gltf_preload_bundle *preload_bundle,
                                            int mesh_index,
                                            int primitive_index,
                                            uint32_t *out_flags) {
    char key[96];
    uint8_t *blob;
    size_t blob_len = 0;
    uint32_t magic;
    uint32_t version;
    uint32_t vertex_count;
    uint32_t index_count;
    uint32_t flags;
    uint32_t vertex_bytes_u32;
    uint32_t index_bytes_u32;
    uint32_t bone_count_u32;
    gltf_skin_import_stats_t skin_stats;
    size_t vertex_bytes;
    size_t index_bytes;
    size_t vertex_offset;
    size_t index_offset;
    size_t required_len;
    vgfx3d_vertex_t *vertices = NULL;
    uint32_t *indices = NULL;
    void *mesh_obj = NULL;
    rt_mesh3d *mesh = NULL;

    if (out_flags)
        *out_flags = 0u;
    gltf_preload_mesh_key(mesh_index, primitive_index, key, sizeof(key));
    blob = gltf_preload_bundle_take_dependency(
        preload_bundle, key, GLTF_PRELOAD_DEP_MESH_POD, &blob_len);
    if (!blob)
        return NULL;
    if (blob_len < GLTF_PRELOAD_MESH_POD_HEADER_SIZE)
        goto malformed;
    magic = gltf_read_u32_le(blob + 0);
    version = gltf_read_u32_le(blob + 4);
    vertex_count = gltf_read_u32_le(blob + 8);
    index_count = gltf_read_u32_le(blob + 12);
    flags = gltf_read_u32_le(blob + 16);
    vertex_bytes_u32 = gltf_read_u32_le(blob + 20);
    index_bytes_u32 = gltf_read_u32_le(blob + 24);
    bone_count_u32 = gltf_read_u32_le(blob + 28);
    skin_stats.truncated_vertices = gltf_read_u32_le(blob + 32);
    skin_stats.out_of_range_vertices = gltf_read_u32_le(blob + 36);
    if (magic != GLTF_PRELOAD_MESH_POD_MAGIC || version != GLTF_PRELOAD_MESH_POD_VERSION ||
        vertex_count == 0 || index_count == 0)
        goto malformed;
    if (bone_count_u32 > VGFX3D_MAX_BONES)
        goto malformed;
    if (!gltf_checked_mul_size((size_t)vertex_count, sizeof(vgfx3d_vertex_t), &vertex_bytes) ||
        !gltf_checked_mul_size((size_t)index_count, sizeof(uint32_t), &index_bytes) ||
        vertex_bytes != (size_t)vertex_bytes_u32 || index_bytes != (size_t)index_bytes_u32)
        goto malformed;
    vertex_offset = GLTF_PRELOAD_MESH_POD_HEADER_SIZE;
    if (!gltf_checked_add_size(vertex_offset, vertex_bytes, &index_offset) ||
        !gltf_checked_add_size(index_offset, index_bytes, &required_len) || required_len > blob_len)
        goto malformed;

    vertices = (vgfx3d_vertex_t *)malloc(vertex_bytes);
    indices = (uint32_t *)malloc(index_bytes);
    if (!vertices || !indices)
        goto malformed;
    memcpy(vertices, blob + vertex_offset, vertex_bytes);
    memcpy(indices, blob + index_offset, index_bytes);

    /* Validate every index against vertex_count before installing. This POD fast path
     * bypasses rt_mesh3d_add_triangle (which range-checks indices on the normal load path),
     * so without this scan a malformed or forged preload blob could leave indices that read
     * out of bounds when a consumer later walks indices[] into vertices[]. */
    {
        uint32_t vc = (uint32_t)vertex_count;
        uint32_t ic = (uint32_t)index_count;
        for (uint32_t vi = 0; vi < ic; vi++) {
            if (indices[vi] >= vc)
                goto malformed;
        }
    }

    mesh_obj = rt_mesh3d_new_empty_storage();
    if (!mesh_obj)
        goto malformed;
    mesh = (rt_mesh3d *)mesh_obj;
    mesh->vertices = vertices;
    vertices = NULL;
    mesh->vertex_count = vertex_count;
    mesh->vertex_capacity = vertex_count;
    mesh->indices = indices;
    indices = NULL;
    mesh->index_count = index_count;
    mesh->index_capacity = index_count;
    mesh->bone_count = (int32_t)bone_count_u32;
    mesh->build_failed = 0;
    rt_mesh3d_touch_geometry_now(mesh);
    if (!gltf_preload_take_decoded_morph(
            preload_bundle, mesh_index, primitive_index, mesh_obj, vertex_count))
        goto malformed;
    /* The worker that decoded this POD could not reach the loader's thread-local
     * warning store, so its skin-import diagnostics ride the header and are
     * surfaced here on the committing thread. */
    gltf_emit_skin_import_warnings(mesh_index, primitive_index, &skin_stats);
    if (out_flags)
        *out_flags = flags;
    free(blob);
    return mesh_obj;

malformed:
    if (mesh_obj) {
        if (rt_obj_release_check0(mesh_obj))
            rt_obj_free(mesh_obj);
    } else {
        free(vertices);
        free(indices);
    }
    free(blob);
    return NULL;
}

/// @brief Return @p value if it is finite, otherwise return @p fallback.
/// @details Protects downstream arithmetic from NaN and ±Inf values that can appear
///   in glTF fields parsed from malformed JSON numbers.
/// @param value Candidate numeric value.
/// @param fallback Replacement used for NaN or either infinity.
/// @return @p value when finite; otherwise @p fallback.
static double gltf_finite_or(double value, double fallback) {
    return isfinite(value) ? value : fallback;
}

/// @brief Sanitise a double: replace non-finite values with @p fallback, then clamp to [lo, hi].
/// @details Combines `gltf_finite_or` with a bounds clamp in one call. Used for material
///   colour components, emissive strength, metallic/roughness factors, and spot-light angles
///   where both NaN/Inf rejection and range enforcement are required.
/// @param value Candidate numeric value.
/// @param lo Inclusive lower bound.
/// @param hi Inclusive upper bound.
/// @param fallback Finite replacement applied before clamping.
/// @return Sanitized value in the inclusive interval [@p lo, @p hi].
static double gltf_clamp_double(double value, double lo, double hi, double fallback) {
    value = gltf_finite_or(value, fallback);
    if (value < lo)
        return lo;
    if (value > hi)
        return hi;
    return value;
}

/// @brief Normalize @p v, or replace it with @p fallback when it is degenerate.
/// @param v Mutable three-component vector.
/// @param fx Fallback X component.
/// @param fy Fallback Y component.
/// @param fz Fallback Z component.
static void gltf_normalize_vec3_or(double *v, double fx, double fy, double fz) {
    double len;
    if (!v)
        return;
    v[0] = gltf_finite_or(v[0], fx);
    v[1] = gltf_finite_or(v[1], fy);
    v[2] = gltf_finite_or(v[2], fz);
    len = sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (!isfinite(len) || len <= 1e-12) {
        v[0] = fx;
        v[1] = fy;
        v[2] = fz;
        return;
    }
    v[0] /= len;
    v[1] /= len;
    v[2] /= len;
}

/// @brief Sanitize a quaternion in place: replace non-finite components (defaulting w to 1),
///   then normalize; falls back to identity (0,0,0,1) when the length is near zero.
/// @param q Mutable XYZW quaternion; may be NULL.
static void gltf_normalize_quat_or_identity(double *q) {
    double len;
    if (!q)
        return;
    q[0] = gltf_finite_or(q[0], 0.0);
    q[1] = gltf_finite_or(q[1], 0.0);
    q[2] = gltf_finite_or(q[2], 0.0);
    q[3] = gltf_finite_or(q[3], 1.0);
    len = sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (!isfinite(len) || len <= 1e-12) {
        q[0] = 0.0;
        q[1] = 0.0;
        q[2] = 0.0;
        q[3] = 1.0;
        return;
    }
    q[0] /= len;
    q[1] /= len;
    q[2] /= len;
    q[3] /= len;
}

/// @brief Sanitize a TRS triple in place: non-finite position components become 0, the
///   quaternion is normalized to identity-on-degenerate, and non-finite scale components
///   become 1. NULL outputs are skipped.
/// @param pos Optional mutable XYZ position.
/// @param quat Optional mutable XYZW rotation quaternion.
/// @param scale Optional mutable XYZ scale.
static void gltf_sanitize_trs(double *pos, double *quat, double *scale) {
    if (pos) {
        pos[0] = gltf_finite_or(pos[0], 0.0);
        pos[1] = gltf_finite_or(pos[1], 0.0);
        pos[2] = gltf_finite_or(pos[2], 0.0);
    }
    gltf_normalize_quat_or_identity(quat);
    if (scale) {
        scale[0] = gltf_finite_or(scale[0], 1.0);
        scale[1] = gltf_finite_or(scale[1], 1.0);
        scale[2] = gltf_finite_or(scale[2], 1.0);
    }
}

/// @brief Read one numeric element from a JSON array by index, with bounds-check and default.
/// @details Combines `jarr_len` bounds-check, `rt_seq_get`, and `jvalue_num` into a single call
///   for safely reading glTF array fields like `"color"` and `"translation"` that may have
///   fewer than the expected number of elements.
/// @param arr JSON array to read.
/// @param index Zero-based element index.
/// @param fallback Value returned for a missing or non-numeric element.
/// @return Parsed numeric value or @p fallback.
static double gltf_arr_num(void *arr, int64_t index, double fallback) {
    if (!arr || index < 0 || index >= jarr_len(arr))
        return fallback;
    return jvalue_num(rt_seq_get(arr, index), fallback);
}

/// @brief Duplicate @p value, or @p fallback when @p value is NULL/empty (traps on alloc failure).
/// @param value Preferred source string.
/// @param fallback Alternate source used when @p value is absent or empty.
/// @return Heap-allocated NUL-terminated copy.
static char *gltf_strdup_or(const char *value, const char *fallback) {
    const char *src = (value && value[0] != '\0') ? value : fallback;
    size_t len;
    char *copy;
    if (!src)
        src = "";
    len = strlen(src);
    if (len == SIZE_MAX) {
        rt_trap("glTF: scene name too long");
        return NULL;
    }
    copy = (char *)malloc(len + 1);
    if (!copy) {
        rt_trap("glTF: scene name allocation failed");
        return NULL;
    }
    memcpy(copy, src, len + 1);
    return copy;
}

/// @brief Append a newly-created Camera3D to the asset-owned active-scene camera list.
/// @details Takes ownership of @p camera on success. On allocation failure the caller still owns
///   the camera and should release it.
/// @param asset Asset whose legacy camera list is extended.
/// @param camera Retained camera handle transferred on success.
/// @return Non-zero on success or a null-argument no-op; zero after a growth failure.
static int gltf_append_camera(rt_gltf_asset *asset, void *camera) {
    void **grown;
    int32_t next_capacity;
    if (!asset || !camera)
        return 1;
    if (asset->camera_count >= asset->camera_capacity) {
        if (asset->camera_capacity > INT32_MAX / 2) {
            rt_trap("glTF: too many cameras");
            return 0;
        }
        next_capacity = asset->camera_capacity > 0 ? asset->camera_capacity * 2 : 4;
        grown = (void **)realloc(asset->cameras, (size_t)next_capacity * sizeof(void *));
        if (!grown) {
            rt_trap("glTF: camera list allocation failed");
            return 0;
        }
        memset(grown + asset->camera_capacity,
               0,
               (size_t)(next_capacity - asset->camera_capacity) * sizeof(*grown));
        asset->cameras = grown;
        asset->camera_capacity = next_capacity;
    }
    asset->cameras[asset->camera_count++] = camera;
    return 1;
}

/// @brief Append a Camera3D to one scene's camera list, growing it as needed (takes ownership).
/// @details Per-scene analogue of gltf_append_camera. Returns 1 on success (or no-op for NULL
///          args); returns 0 and traps on overflow or allocation failure.
/// @param scene Scene record whose camera array is extended.
/// @param camera Retained camera handle transferred on success.
/// @return Non-zero on success or a null-argument no-op; zero after a growth failure.
static int gltf_append_scene_camera(gltf_scene_info_t *scene, void *camera) {
    void **grown;
    int32_t next_capacity;
    if (!scene || !camera)
        return 1;
    if (scene->camera_count >= scene->camera_capacity) {
        if (scene->camera_capacity > INT32_MAX / 2) {
            rt_trap("glTF: too many scene cameras");
            return 0;
        }
        next_capacity = scene->camera_capacity > 0 ? scene->camera_capacity * 2 : 4;
        grown = (void **)realloc(scene->cameras, (size_t)next_capacity * sizeof(void *));
        if (!grown) {
            rt_trap("glTF: scene camera list allocation failed");
            return 0;
        }
        memset(grown + scene->camera_capacity,
               0,
               (size_t)(next_capacity - scene->camera_capacity) * sizeof(*grown));
        scene->cameras = grown;
        scene->camera_capacity = next_capacity;
    }
    scene->cameras[scene->camera_count++] = camera;
    return 1;
}

/// @brief Register a scene (its root node + name) on the asset, growing the scene list.
/// @details Counts the root's descendants for node_count and synthesizes a name ("default" for
///          the first scene, else "scene_N") when @p name is empty. Returns 0 and traps on overflow
///          or allocation failure; the root is borrowed (the asset references it, not copies it).
/// @param asset Asset whose immutable scene list is extended.
/// @param root Borrowed scene root node stored in the new record.
/// @param name Optional source scene name.
/// @return Non-zero after the scene record and owned name are installed.
static int gltf_append_scene(rt_gltf_asset *asset, void *root, const char *name) {
    gltf_scene_info_t *grown;
    gltf_scene_info_t *scene;
    int32_t next_capacity;
    char fallback[64];
    if (!asset || !root)
        return 0;
    if (asset->scene_count >= asset->scene_capacity) {
        if (asset->scene_capacity > INT32_MAX / 2) {
            rt_trap("glTF: too many scenes");
            return 0;
        }
        next_capacity = asset->scene_capacity > 0 ? asset->scene_capacity * 2 : 4;
        grown = (gltf_scene_info_t *)realloc(asset->scenes,
                                             (size_t)next_capacity * sizeof(*asset->scenes));
        if (!grown) {
            rt_trap("glTF: scene list allocation failed");
            return 0;
        }
        memset(grown + asset->scene_capacity,
               0,
               (size_t)(next_capacity - asset->scene_capacity) * sizeof(*grown));
        asset->scenes = grown;
        asset->scene_capacity = next_capacity;
    }
    snprintf(fallback,
             sizeof(fallback),
             asset->scene_count == 0 ? "default" : "scene_%d",
             (int)asset->scene_count);
    scene = &asset->scenes[asset->scene_count];
    memset(scene, 0, sizeof(*scene));
    scene->root = root;
    scene->node_count = gltf_count_subtree((rt_scene_node3d *)root) - 1;
    scene->name = gltf_strdup_or(name, fallback);
    if (!scene->name) {
        memset(scene, 0, sizeof(*scene));
        return 0;
    }
    asset->scene_count++;
    return 1;
}

/// @brief Clone a scene node's own transform/bounds/attachments without its children.
/// @details Copies the local TRS, AABB, and bounding sphere, and shares (retains) the mesh,
///          material, light, and name handles. Marks world transform dirty. Children are not
///          copied — gltf_clone_scene_node walks the hierarchy using this as the per-node step.
/// @param src Source scene node whose local state and attachments are copied.
/// @return Newly retained shallow clone, or NULL on allocation or material-variant copy failure.
static rt_scene_node3d *gltf_clone_scene_node_shallow(const rt_scene_node3d *src) {
    rt_scene_node3d *dst;
    if (!src)
        return NULL;
    dst = (rt_scene_node3d *)rt_scene_node3d_new();
    if (!dst)
        return NULL;
    memcpy(dst->position, src->position, sizeof(dst->position));
    memcpy(dst->rotation, src->rotation, sizeof(dst->rotation));
    memcpy(dst->scale_xyz, src->scale_xyz, sizeof(dst->scale_xyz));
    memcpy(dst->aabb_min, src->aabb_min, sizeof(dst->aabb_min));
    memcpy(dst->aabb_max, src->aabb_max, sizeof(dst->aabb_max));
    dst->bsphere_radius = src->bsphere_radius;
    dst->world_dirty = 1;
    dst->import_index = src->import_index;
    dst->visible = src->visible;
    if (src->mesh) {
        rt_obj_retain_maybe(src->mesh);
        dst->mesh = src->mesh;
    }
    if (src->material) {
        rt_obj_retain_maybe(src->material);
        dst->material = src->material;
    }
    if (src->light) {
        rt_obj_retain_maybe(src->light);
        dst->light = src->light;
    }
    if (src->name) {
        rt_obj_retain_maybe(src->name);
        dst->name = src->name;
    }
    /* Prefab identity survives glTF-path clones too (ADR 0187). */
    if (src->prefab_path) {
        rt_obj_retain_maybe(src->prefab_path);
        dst->prefab_path = src->prefab_path;
    }
    dst->is_instance_content = src->is_instance_content ? 1 : 0;
    if (!rt_scene_node3d_copy_variant_materials(dst, src)) {
        gltf_release_local(dst);
        return NULL;
    }
    return dst;
}

/// @brief One explicit-stack frame for the iterative scene-node clone: the source/dest node
///        pair and the index of the next child still to be cloned.
typedef struct {
    /// Borrowed source node currently being traversed.
    const rt_scene_node3d *src;
    /// Corresponding destination clone.
    rt_scene_node3d *dst;
    /// Index of the next source child to clone.
    int32_t next_child;
} gltf_clone_frame_t;

/// @brief Deep-clone a scene node and its entire subtree (shallow-clone per node + shared assets).
/// @details Uses an explicit growable stack rather than recursion, so arbitrarily deep glTF
///          hierarchies cannot overflow the C call stack. On any allocation failure it releases
///          the partially-built clone and traps. Returns the cloned root (NULL on failure).
/// @param src Root of the source hierarchy to clone.
/// @return Newly retained cloned root, or NULL on allocation or child-attachment failure.
static rt_scene_node3d *gltf_clone_scene_node(const rt_scene_node3d *src) {
    rt_scene_node3d *root;
    gltf_clone_frame_t *stack;
    int32_t stack_count = 0;
    int32_t stack_capacity = 32;
    if (!src)
        return NULL;
    root = gltf_clone_scene_node_shallow(src);
    if (!root)
        return NULL;
    stack = (gltf_clone_frame_t *)malloc((size_t)stack_capacity * sizeof(*stack));
    if (!stack) {
        rt_trap("glTF: scene clone stack allocation failed");
        gltf_release_local(root);
        return NULL;
    }
    stack[stack_count++] = (gltf_clone_frame_t){src, root, 0};
    while (stack_count > 0) {
        gltf_clone_frame_t *frame = &stack[stack_count - 1];
        const rt_scene_node3d *src_child;
        rt_scene_node3d *dst_child;
        if (!frame->src || frame->next_child >= frame->src->child_count) {
            stack_count--;
            continue;
        }
        src_child = frame->src->children[frame->next_child++];
        dst_child = gltf_clone_scene_node_shallow(src_child);
        if (!dst_child) {
            free(stack);
            gltf_release_local(root);
            return NULL;
        }
        if (!rt_scene_node3d_try_add_child(frame->dst, dst_child)) {
            gltf_release_local(dst_child);
            free(stack);
            gltf_release_local(root);
            return NULL;
        }
        gltf_release_local(dst_child);
        if (stack_count >= stack_capacity) {
            gltf_clone_frame_t *grown;
            int32_t next_capacity;
            if (stack_capacity > INT32_MAX / 2) {
                free(stack);
                gltf_release_local(root);
                rt_trap("glTF: too many scene nodes to clone");
                return NULL;
            }
            next_capacity = stack_capacity * 2;
            grown = (gltf_clone_frame_t *)realloc(stack, (size_t)next_capacity * sizeof(*stack));
            if (!grown) {
                free(stack);
                gltf_release_local(root);
                rt_trap("glTF: scene clone stack allocation failed");
                return NULL;
            }
            stack = grown;
            stack_capacity = next_capacity;
        }
        stack[stack_count++] = (gltf_clone_frame_t){src_child, dst_child, 0};
    }
    free(stack);
    return root;
}

/// @brief Build a Camera3D from one glTF `cameras[]` JSON entry.
/// @details glTF perspective `yfov` is radians; Zanna stores vertical FOV in degrees. glTF
///   orthographic `xmag`/`ymag` are view half-extents, matching Camera3D.NewOrtho's half-height
///   size parameter and aspect ratio.
/// @param camera_json One glTF `cameras` array entry.
/// @return Newly retained perspective or orthographic camera, or NULL for an invalid type.
static void *gltf_make_camera(void *camera_json) {
    static const double pi = 3.14159265358979323846;
    const char *type = jstr(camera_json, "type");
    if (!camera_json || !type)
        return NULL;
    if (strcmp(type, "perspective") == 0) {
        void *persp = jget(camera_json, "perspective");
        double znear = gltf_clamp_double(jvalue_num(jget(persp, "znear"), 0.1), 1e-4, DBL_MAX, 0.1);
        double zfar =
            gltf_finite_or(jvalue_num(jget(persp, "zfar"), znear + 1000.0), znear + 1000.0);
        double yfov = gltf_clamp_double(
            jvalue_num(jget(persp, "yfov"), pi / 3.0), pi / 180.0, 179.0 * pi / 180.0, pi / 3.0);
        double aspect =
            gltf_clamp_double(jvalue_num(jget(persp, "aspectRatio"), 1.0), 1e-6, DBL_MAX, 1.0);
        if (zfar <= znear + 1e-4)
            zfar = znear + 1000.0;
        return rt_camera3d_new(yfov * (180.0 / pi), aspect, znear, zfar);
    }
    if (strcmp(type, "orthographic") == 0) {
        void *ortho = jget(camera_json, "orthographic");
        double xmag = fabs(jvalue_num(jget(ortho, "xmag"), 1.0));
        double ymag = fabs(jvalue_num(jget(ortho, "ymag"), 1.0));
        double znear = gltf_clamp_double(jvalue_num(jget(ortho, "znear"), 0.1), 1e-4, DBL_MAX, 0.1);
        double zfar =
            gltf_finite_or(jvalue_num(jget(ortho, "zfar"), znear + 1000.0), znear + 1000.0);
        double aspect;
        if (!isfinite(xmag) || xmag <= 1e-9)
            xmag = 1.0;
        if (!isfinite(ymag) || ymag <= 1e-9)
            ymag = 1.0;
        if (zfar <= znear + 1e-4)
            zfar = znear + 1000.0;
        aspect = xmag / ymag;
        return rt_camera3d_new_ortho(ymag, aspect, znear, zfar);
    }
    return NULL;
}

/// @brief Apply a glTF camera node's world transform to a standalone Camera3D object.
/// @details glTF cameras look down local -Z with +Y as up. SceneNode3D matrices are row-major,
///   so the local -Z and +Y axes are read from the third and second rotation columns.
/// @param camera Runtime camera whose eye, target, and up vector are updated.
/// @param node Imported scene node carrying the camera's world transform.
static void gltf_apply_camera_node_transform(void *camera, rt_scene_node3d *node) {
    void *world_obj;
    double m[16];
    double eye[3];
    double forward[3];
    double up[3];
    double target[3];
    if (!camera || !node)
        return;
    world_obj = rt_scene_node3d_get_world_matrix(node);
    if (!world_obj)
        return;
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
            m[r * 4 + c] = rt_mat4_get(world_obj, r, c);
    gltf_release_ref(&world_obj);

    eye[0] = m[3];
    eye[1] = m[7];
    eye[2] = m[11];
    forward[0] = -m[2];
    forward[1] = -m[6];
    forward[2] = -m[10];
    up[0] = m[1];
    up[1] = m[5];
    up[2] = m[9];
    gltf_normalize_vec3_or(forward, 0.0, 0.0, -1.0);
    gltf_normalize_vec3_or(up, 0.0, 1.0, 0.0);
    target[0] = eye[0] + forward[0];
    target[1] = eye[1] + forward[1];
    target[2] = eye[2] + forward[2];
    rt_camera3d_look_at_components(
        camera, eye[0], eye[1], eye[2], target[0], target[1], target[2], up[0], up[1], up[2]);
}

/// @brief Mark @p root_idx and its descendants as members of the active scene.
/// @param nodes_arr glTF JSON `nodes` array.
/// @param node_count Number of entries in @p nodes_arr and @p active_nodes.
/// @param root_idx Root node index from which traversal begins.
/// @param active_nodes Mutable byte flags indexed by glTF node.
/// @return Non-zero when the iterative traversal completes without allocation overflow.
static int gltf_mark_active_node_subtree(void *nodes_arr,
                                         int32_t node_count,
                                         int32_t root_idx,
                                         uint8_t *active_nodes) {
    int32_t *stack = NULL;
    int32_t stack_count = 0;
    int32_t stack_capacity = 32;
    if (!nodes_arr || !active_nodes || root_idx < 0 || root_idx >= node_count)
        return 0;
    stack = (int32_t *)malloc((size_t)stack_capacity * sizeof(*stack));
    if (!stack)
        return 0;
    stack[stack_count++] = root_idx;
    while (stack_count > 0) {
        int32_t node_idx = stack[--stack_count];
        void *node_json;
        void *children;
        if (node_idx < 0 || node_idx >= node_count)
            continue;
        if (active_nodes[node_idx])
            continue;
        active_nodes[node_idx] = 1;
        node_json = rt_seq_get(nodes_arr, node_idx);
        children = jarr(node_json, "children");
        for (int64_t ci = 0; ci < jarr_len(children); ci++) {
            int64_t child_idx = jvalue_int(rt_seq_get(children, ci), -1);
            if (child_idx < 0 || child_idx >= node_count)
                continue;
            if (stack_count >= stack_capacity) {
                int32_t next_capacity;
                int32_t *grown;
                if (stack_capacity > INT32_MAX / 2) {
                    free(stack);
                    return 0;
                }
                next_capacity = stack_capacity * 2;
                grown = (int32_t *)realloc(stack, (size_t)next_capacity * sizeof(*stack));
                if (!grown) {
                    free(stack);
                    return 0;
                }
                stack = grown;
                stack_capacity = next_capacity;
            }
            stack[stack_count++] = (int32_t)child_idx;
        }
    }
    free(stack);
    return 1;
}

/// @brief Create Camera3D objects for camera nodes that are reachable from one immutable scene.
/// @param scene Scene record receiving cameras and node attachments.
/// @param root glTF JSON root containing camera definitions.
/// @param nodes_arr glTF JSON `nodes` array.
/// @param node_count Number of node slots covered by the scene maps.
/// @param active_nodes Membership flags for this scene.
/// @return Non-zero when all reachable camera nodes are resolved and installed.
static int gltf_import_scene_cameras(gltf_scene_info_t *scene,
                                     void *root,
                                     void *nodes_arr,
                                     int32_t node_count,
                                     const uint8_t *active_nodes) {
    void *cameras_arr = jarr(root, "cameras");
    int64_t camera_def_count = jarr_len(cameras_arr);
    rt_scene_node3d **scene_nodes = NULL;
    rt_scene_node3d **stack = NULL;
    size_t stack_count = 0;
    size_t stack_capacity;
    int result = 1;
    if (!scene || !nodes_arr || !active_nodes || camera_def_count <= 0)
        return 1;
    if (!scene->root || node_count <= 0 || (size_t)node_count > SIZE_MAX / sizeof(*scene_nodes) ||
        (size_t)node_count >= SIZE_MAX / sizeof(*stack))
        return 0;
    scene_nodes = (rt_scene_node3d **)calloc((size_t)node_count, sizeof(*scene_nodes));
    stack_capacity = (size_t)node_count + 1u;
    stack = (rt_scene_node3d **)malloc(stack_capacity * sizeof(*stack));
    if (!scene_nodes || !stack) {
        free(stack);
        free(scene_nodes);
        return 0;
    }
    stack[stack_count++] = (rt_scene_node3d *)scene->root;
    while (stack_count > 0) {
        rt_scene_node3d *node = stack[--stack_count];
        int32_t child_count;
        if (!node)
            continue;
        if (node->import_index >= 0 && node->import_index < node_count) {
            if (scene_nodes[node->import_index]) {
                result = 0;
                break;
            }
            scene_nodes[node->import_index] = node;
        }
        child_count = scene3d_node_child_count(node);
        if ((size_t)child_count > stack_capacity - stack_count) {
            result = 0;
            break;
        }
        for (int32_t child = 0; child < child_count; ++child)
            stack[stack_count++] = node->children[child];
    }
    if (!result)
        goto done;
    for (int32_t ni = 0; ni < node_count; ni++) {
        void *node_json;
        int64_t camera_ref;
        void *camera;
        rt_scene_node3d *camera_node;
        if (!active_nodes[ni])
            continue;
        node_json = rt_seq_get(nodes_arr, ni);
        camera_ref = jvalue_int(jget(node_json, "camera"), -1);
        if (camera_ref < 0 || camera_ref >= camera_def_count)
            continue;
        camera_node = scene_nodes[ni];
        if (!camera_node) {
            result = 0;
            break;
        }
        camera = gltf_make_camera(rt_seq_get(cameras_arr, camera_ref));
        if (!camera)
            continue;
        gltf_apply_camera_node_transform(camera, camera_node);
        if (!gltf_append_scene_camera(scene, camera)) {
            gltf_release_ref(&camera);
            result = 0;
            break;
        }
        rt_scene_node3d_set_camera(camera_node, camera);
    }
done:
    free(stack);
    free(scene_nodes);
    return result;
}

/// @brief Mirror the first parsed scene onto the asset's legacy single-scene fields.
/// @details Back-compat shim: retains scene 0's root as the asset's scene_root, copies its node
///          count, and re-registers its cameras on the asset so older single-scene accessors keep
///          working. Returns 0 and releases the camera on append failure.
/// @param asset Asset whose legacy fields are populated from its first scene.
/// @return Non-zero on success or when the asset has no scene; zero on camera-list growth failure.
static int gltf_install_active_scene_compat(rt_gltf_asset *asset) {
    gltf_scene_info_t *scene;
    if (!asset || asset->scene_count <= 0)
        return 1;
    scene = &asset->scenes[0];
    if (scene->root) {
        rt_obj_retain_maybe(scene->root);
        asset->scene_root = scene->root;
        asset->node_count = scene->node_count;
    }
    for (int32_t i = 0; i < scene->camera_count; i++) {
        void *camera = scene->cameras[i];
        if (!camera)
            continue;
        rt_obj_retain_maybe(camera);
        if (!gltf_append_camera(asset, camera)) {
            gltf_release_ref(&camera);
            return 0;
        }
    }
    return 1;
}

/// @brief Convert a glTF punctual-light range into the runtime local-light attenuation floor.
/// @details glTF omits `range` or sets it to zero for unbounded local lights. Zanna intentionally
/// avoids no-falloff point/spot lights because they are visually surprising and expensive, so this
/// helper maps omitted, invalid, overflowing, or too-small attenuation values to the same floor
/// used by `Light3D.NewPoint` and `Light3D.NewSpot`.
/// @param range glTF light range in scene units.
/// @return Runtime attenuation coefficient for point and spot lights.
static double gltf_local_light_attenuation(double range) {
    double attenuation;
    if (!isfinite(range) || range <= 1e-6)
        return RT_LIGHT3D_DEFAULT_ATTENUATION;
    attenuation = 1.0 / (range * range);
    if (!isfinite(attenuation) || attenuation < RT_LIGHT3D_DEFAULT_ATTENUATION)
        return RT_LIGHT3D_DEFAULT_ATTENUATION;
    return attenuation;
}

/// @brief Construct a `rt_light3d` from a `KHR_lights_punctual` extension light JSON object.
/// @details Reads `type` (directional/point/spot), `color`, `intensity`, and `range` from
///   the light JSON. Intensity is stored directly; range is converted to an inverse-square
///   attenuation factor (`1 / range²`) for point/spot lights with the runtime local-light floor.
///   For spot lights, `innerConeAngle` and `outerConeAngle` are read from the nested `"spot"`
///   object, clamped to `(0, π/2]`, and stored as their cosines — the convention expected by
///   the shader. A minimum angular gap (`spot_eps`) is enforced between inner and outer angles
///   to prevent the penumbra from collapsing to zero width. The default direction `(0, 0, −1)`
///   matches the glTF spec's local-space forward vector; the actual world-space direction is
///   applied when the node's transform is combined with the light during scene-graph traversal.
/// @param light_json  Parsed KHR_lights_punctual light JSON object; must be non-NULL.
/// @return Newly allocated `rt_light3d` on success; NULL if the type field is absent or unknown.
static void *gltf_new_punctual_light(void *light_json) {
    static const double pi = 3.14159265358979323846;
    const char *type = jstr(light_json, "type");
    void *color_arr = jarr(light_json, "color");
    void *spot = jget(light_json, "spot");
    double range = jnum(light_json, "range", 0.0);
    double inner_angle = spot ? jnum(spot, "innerConeAngle", 0.0) : 0.0;
    double outer_angle = spot ? jnum(spot, "outerConeAngle", pi / 4.0) : pi / 4.0;
    rt_light3d *light;
    if (!type)
        return NULL;
    light = (rt_light3d *)rt_obj_new_i64(RT_G3D_LIGHT3D_CLASS_ID, (int64_t)sizeof(rt_light3d));
    if (!light)
        return NULL;
    memset(light, 0, sizeof(*light));
    light->color[0] = gltf_clamp_double(gltf_arr_num(color_arr, 0, 1.0), 0.0, 1.0, 1.0);
    light->color[1] = gltf_clamp_double(gltf_arr_num(color_arr, 1, 1.0), 0.0, 1.0, 1.0);
    light->color[2] = gltf_clamp_double(gltf_arr_num(color_arr, 2, 1.0), 0.0, 1.0, 1.0);
    light->intensity = gltf_clamp_double(jnum(light_json, "intensity", 1.0), 0.0, DBL_MAX, 1.0);
    light->attenuation = 0.0;
    light->direction[0] = 0.0;
    light->direction[1] = 0.0;
    light->direction[2] = -1.0;
    light->enabled = 1;
    light->casts_shadows = 0;

    if (strcmp(type, "directional") == 0) {
        light->type = 0;
        light->casts_shadows = 1;
        return light;
    }
    if (strcmp(type, "point") == 0) {
        light->type = 1;
        light->attenuation = gltf_local_light_attenuation(range);
        return light;
    }
    if (strcmp(type, "spot") == 0) {
        const double max_spot_angle = pi * 0.5;
        const double spot_eps = 1e-4;
        light->type = 3;
        light->attenuation = gltf_local_light_attenuation(range);
        inner_angle = gltf_clamp_double(inner_angle, 0.0, max_spot_angle - spot_eps, 0.0);
        outer_angle = gltf_clamp_double(outer_angle, 0.0, max_spot_angle, pi / 4.0);
        if (outer_angle <= inner_angle + spot_eps) {
            if (inner_angle > max_spot_angle - spot_eps)
                inner_angle = max_spot_angle - spot_eps;
            outer_angle = inner_angle + spot_eps;
        }
        if (outer_angle > max_spot_angle) {
            outer_angle = max_spot_angle;
            if (inner_angle >= outer_angle)
                inner_angle = outer_angle - spot_eps;
        }
        light->inner_cos = cos(inner_angle);
        light->outer_cos = cos(outer_angle);
        return light;
    }

    gltf_release_local(light);
    return NULL;
}

/// @brief Parse the KHR_lights_punctual extension block from the glTF root and build a light array.
/// @details Walks `root.extensions.KHR_lights_punctual.lights`, calls `gltf_new_punctual_light`
///          for each entry, and writes the resulting pointer array and count to the output params.
///          On failure or missing extension both outputs are left at NULL/0.
/// @param root glTF JSON root.
/// @param out_lights Receives a heap-allocated array of retained light handles.
/// @param out_count Receives the number of array slots, including any null entries.
static void gltf_parse_punctual_lights(void *root, void ***out_lights, int32_t *out_count) {
    void *extensions;
    void *punctual;
    void *lights_arr;
    int64_t count64;
    void **lights;
    if (out_lights)
        *out_lights = NULL;
    if (out_count)
        *out_count = 0;
    if (!root || !out_lights || !out_count)
        return;
    extensions = jget(root, "extensions");
    punctual = extensions ? jget(extensions, "KHR_lights_punctual") : NULL;
    lights_arr = punctual ? jarr(punctual, "lights") : NULL;
    count64 = jarr_len(lights_arr);
    if (!lights_arr || count64 <= 0 || count64 > INT32_MAX ||
        !rt_untrusted_count_ok(count64, sizeof(void *), 64u * 1024u * 1024u))
        return;
    lights = (void **)calloc((size_t)count64, sizeof(void *));
    if (!lights)
        return;
    for (int64_t i = 0; i < count64; i++)
        lights[i] = gltf_new_punctual_light(rt_seq_get(lights_arr, i));
    *out_lights = lights;
    *out_count = (int32_t)count64;
}

/// @brief Retrieve a morph-target name from the mesh's `extras.targetNames` array.
/// @details If no name is present (missing extras, out-of-range index, or empty string),
///          fills @p fallback with `"target_N"` and returns it so the caller always
///          gets a non-NULL, non-empty string.
/// @param mesh_json glTF mesh object containing optional target-name extras.
/// @param target_index Morph-target index to name.
/// @param fallback Writable buffer for the generated name.
/// @param fallback_cap Capacity of @p fallback including its terminator.
/// @return Borrowed JSON string when available; otherwise @p fallback.
static const char *gltf_target_name(void *mesh_json,
                                    int32_t target_index,
                                    char *fallback,
                                    size_t fallback_cap) {
    void *extras;
    void *names;
    rt_string name;
    const char *cstr;
    if (fallback && fallback_cap > 0) {
        snprintf(fallback, fallback_cap, "target_%d", (int)target_index);
        fallback[fallback_cap - 1] = '\0';
    }
    extras = jget(mesh_json, "extras");
    names = extras ? jarr(extras, "targetNames") : NULL;
    if (!names || target_index < 0 || target_index >= jarr_len(names))
        return fallback;
    name = (rt_string)rt_seq_get(names, target_index);
    cstr = name ? rt_string_cstr(name) : NULL;
    return cstr && cstr[0] != '\0' ? cstr : fallback;
}

/// @brief Read all morph targets from a glTF primitive's `targets` array into a MorphTarget3D
/// object.
/// @details Creates a `rt_morphtarget3d` with @p vertex_count slots, iterates each target
///          entry, reads POSITION / NORMAL / TANGENT accessor deltas, and registers them as
///          named shapes. Initial weights come from the mesh-level `weights` array when present.
///          The morph target is attached to @p mesh_obj via `rt_mesh3d_set_morph_targets`.
/// @param root glTF JSON root used for accessor lookup.
/// @param buffers Materialized glTF buffers.
/// @param buf_count Number of entries in @p buffers.
/// @param mesh_json Parent mesh object supplying names and default weights.
/// @param prim Primitive JSON object containing the target accessors.
/// @param mesh_obj Runtime mesh receiving the morph-target object.
/// @param vertex_count Number of vertex slots in the runtime mesh.
static void gltf_import_primitive_morph_targets(void *root,
                                                gltf_buffer_t *buffers,
                                                int buf_count,
                                                void *mesh_json,
                                                void *prim,
                                                void *mesh_obj,
                                                int32_t vertex_count) {
    void *targets;
    void *mesh_weights;
    void *morph = NULL;
    int64_t target_count;
    if (!root || !prim || !mesh_obj || vertex_count <= 0)
        return;
    targets = jarr(prim, "targets");
    target_count = jarr_len(targets);
    if (!targets || target_count <= 0)
        return;

    morph = rt_morphtarget3d_new(vertex_count);
    if (!morph)
        return;
    mesh_weights = mesh_json ? jarr(mesh_json, "weights") : NULL;

    for (int64_t ti = 0; ti < target_count; ti++) {
        void *target = rt_seq_get(targets, ti);
        int64_t pos_acc = jint(target, "POSITION", -1);
        int64_t norm_acc = jint(target, "NORMAL", -1);
        int64_t tangent_acc = jint(target, "TANGENT", -1);
        gltf_accessor_view_t pos_view;
        gltf_accessor_view_t norm_view;
        gltf_accessor_view_t tangent_view;
        int has_pos = gltf_get_accessor_view(root, pos_acc, buffers, buf_count, &pos_view);
        int has_norm = gltf_get_accessor_view(root, norm_acc, buffers, buf_count, &norm_view);
        int has_tangent =
            gltf_get_accessor_view(root, tangent_acc, buffers, buf_count, &tangent_view);
        if (has_pos &&
            (!gltf_accessor_has_payload(&pos_view) || !gltf_accessor_is_position(&pos_view)))
            has_pos = 0;
        if (has_norm &&
            (!gltf_accessor_has_payload(&norm_view) || !gltf_accessor_is_normal(&norm_view)))
            has_norm = 0;
        if (has_tangent && (!gltf_accessor_has_payload(&tangent_view) ||
                            !gltf_accessor_is_tangent(&tangent_view, 3)))
            has_tangent = 0;
        char fallback_name[64];
        const char *name;
        int64_t shape;
        if (!has_pos && !has_norm && !has_tangent)
            continue;

        name = gltf_target_name(mesh_json, (int32_t)ti, fallback_name, sizeof(fallback_name));
        {
            rt_string runtime_name = rt_const_cstr(name);
            shape = rt_morphtarget3d_add_shape(morph, runtime_name);
            rt_string_unref(runtime_name);
        }
        if (shape < 0)
            continue;

        if (mesh_weights && ti < jarr_len(mesh_weights))
            rt_morphtarget3d_set_weight(
                morph, shape, jvalue_num(rt_seq_get(mesh_weights, ti), 0.0));

        if (has_pos) {
            int32_t limit = pos_view.count < vertex_count ? pos_view.count : vertex_count;
            for (int32_t vi = 0; vi < limit; vi++) {
                float delta[3] = {0.0f, 0.0f, 0.0f};
                gltf_accessor_read_f32(&pos_view, vi, delta, 3);
                rt_morphtarget3d_set_delta(morph, shape, vi, delta[0], delta[1], delta[2]);
            }
        }
        if (has_norm) {
            int32_t limit = norm_view.count < vertex_count ? norm_view.count : vertex_count;
            for (int32_t vi = 0; vi < limit; vi++) {
                float delta[3] = {0.0f, 0.0f, 0.0f};
                gltf_accessor_read_f32(&norm_view, vi, delta, 3);
                rt_morphtarget3d_set_normal_delta(morph, shape, vi, delta[0], delta[1], delta[2]);
            }
        }
        if (has_tangent) {
            int32_t limit = tangent_view.count < vertex_count ? tangent_view.count : vertex_count;
            for (int32_t vi = 0; vi < limit; vi++) {
                float delta[3] = {0.0f, 0.0f, 0.0f};
                gltf_accessor_read_f32(&tangent_view, vi, delta, 3);
                rt_morphtarget3d_set_tangent_delta(morph, shape, vi, delta[0], delta[1], delta[2]);
            }
        }
    }

    if (rt_morphtarget3d_get_shape_count(morph) > 0)
        rt_mesh3d_set_morph_targets(mesh_obj, morph);
    gltf_release_local(morph);
}

/// @brief Write per-shape weights from a glTF node's `weights` array into the mesh's morph target.
/// @details Iterates up to `min(shape_count, weights_arr length)` entries and calls
///          `rt_morphtarget3d_set_weight` for each. No-op when the mesh has no morph targets
///          or when @p weights_arr is NULL.
/// @param mesh_obj Runtime mesh whose attached morph target is updated.
/// @param weights_arr JSON node-level weights array.
static void gltf_apply_node_morph_weights(void *mesh_obj, void *weights_arr) {
    rt_mesh3d *mesh = (rt_mesh3d *)mesh_obj;
    int64_t shape_count;
    int64_t limit;
    if (!mesh || !mesh->morph_targets_ref || !weights_arr)
        return;
    shape_count = rt_morphtarget3d_get_shape_count(mesh->morph_targets_ref);
    limit = jarr_len(weights_arr);
    if (limit < 0)
        return;
    if (limit > shape_count)
        limit = shape_count;
    for (int64_t i = 0; i < limit; i++)
        rt_morphtarget3d_set_weight(
            mesh->morph_targets_ref, i, jvalue_num(rt_seq_get(weights_arr, i), 0.0));
    for (int64_t i = limit; i < shape_count; i++)
        rt_morphtarget3d_set_weight(mesh->morph_targets_ref, i, 0.0);
}

/// @brief Combine a glTF document's directory with a relative URI to get an absolute filesystem
/// path.
///
/// External buffers and textures in glTF are referenced via paths
/// relative to the .gltf file itself; this prepends the document's
/// directory so the loader can open them.
/// @param ch Candidate ASCII hexadecimal digit.
/// @return Numeric value from zero through fifteen, or -1 when invalid.
static int gltf_hex_digit(char ch) {
    if (ch >= '0' && ch <= '9')
        return ch - '0';
    if (ch >= 'a' && ch <= 'f')
        return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F')
        return ch - 'A' + 10;
    return -1;
}

/// @brief Decode a percent-encoded URI path component into a plain filesystem path string.
/// @details Converts `%XX` escape sequences to their byte values, stops at `#` (fragment)
///          or `?` (query), and NUL-terminates @p out. Rejects malformed escapes and any control
///          character (< 0x20, including NUL) or DEL (0x7F), whether literal or percent-decoded.
///          Does not handle scheme prefixes (file://, http://) — caller rejects those after decode.
/// @param uri NUL-terminated URI path to decode.
/// @param out Destination buffer for the decoded path.
/// @param out_cap Capacity of @p out including its terminator.
/// @return Non-zero when a non-empty safe path was written.
static int gltf_decode_uri_path(const char *uri, char *out, size_t out_cap) {
    size_t oi = 0;
    if (!out || out_cap == 0)
        return 0;
    out[0] = '\0';
    if (!uri)
        return 0;
    while (*uri) {
        unsigned char decoded;
        if (*uri == '#' || *uri == '?')
            break;
        if (oi + 1 >= out_cap)
            return 0;
        if (*uri == '%') {
            int hi = gltf_hex_digit(uri[1]);
            int lo = gltf_hex_digit(uri[2]);
            if (hi >= 0 && lo >= 0) {
                decoded = (unsigned char)((hi << 4) | lo);
                if (decoded < 0x20u || decoded == 0x7Fu)
                    return 0;
                out[oi++] = (char)decoded;
                uri += 3;
                continue;
            }
            return 0;
        }
        if (*uri == '\0' || (unsigned char)*uri < 0x20u || (unsigned char)*uri == 0x7Fu)
            return 0;
        out[oi++] = *uri++;
    }
    out[oi] = '\0';
    return oi > 0;
}

/// @brief Decode a glTF URI path into a heap buffer sized from the input.
/// @details The decoded path is still bounded by `GLTF_MAX_URI_PATH_BYTES`, rejects raw or
///          percent-decoded control characters, and strips query/fragment suffixes just like
///          `gltf_decode_uri_path`.
/// @param uri NUL-terminated URI path to decode.
/// @return Malloc-owned decoded path, or NULL on malformed/empty/overlong input.
static char *gltf_decode_uri_path_alloc(const char *uri) {
    size_t uri_len;
    char *decoded;
    if (!uri)
        return NULL;
    uri_len = 0;
    while (uri[uri_len] != '\0' && uri[uri_len] != '?' && uri[uri_len] != '#') {
        if (uri_len >= GLTF_MAX_URI_PATH_BYTES)
            return NULL;
        ++uri_len;
    }
    if (uri_len == 0 || uri_len > GLTF_MAX_URI_PATH_BYTES)
        return NULL;
    decoded = (char *)malloc(uri_len + 1u);
    if (!decoded)
        return NULL;
    if (!gltf_decode_uri_path(uri, decoded, uri_len + 1u)) {
        free(decoded);
        return NULL;
    }
    return decoded;
}

/// @brief Combine a glTF document's base path with a relative URI into a malloc-owned path.
/// @details Decodes the URI, normalizes safe `.` path segments, rejects absolute paths, URI
///          schemes, `..` traversal, decoded NULs, and overlong results, then prepends the
///          directory component of @p base_path. Both `/` and `\` separators are recognised.
/// @param base_path Filesystem or asset path of the root glTF document.
/// @param uri Relative dependency URI from the document.
/// @return Malloc-owned resolved path, or NULL when the URI is unsafe, malformed, or too long.
static char *gltf_resolve_relative_path_alloc(const char *base_path, const char *uri) {
    char *decoded_uri;
    char *normalized_uri;
    char *resolved;
    const char *last_sep;
    const char *last_bsep;
    size_t dir_len;
    size_t normalized_len;
    size_t total_len;
    decoded_uri = gltf_decode_uri_path_alloc(uri);
    if (!decoded_uri)
        return NULL;
    normalized_uri = (char *)malloc(strlen(decoded_uri) + 1u);
    if (!normalized_uri) {
        free(decoded_uri);
        return NULL;
    }
    if (!gltf_normalize_relative_uri(decoded_uri, normalized_uri, strlen(decoded_uri) + 1u)) {
        free(decoded_uri);
        free(normalized_uri);
        return NULL;
    }
    free(decoded_uri);
    last_sep = strrchr(base_path ? base_path : "", '/');
    last_bsep = strrchr(base_path ? base_path : "", '\\');
    if (last_bsep && (!last_sep || last_bsep > last_sep))
        last_sep = last_bsep;
    normalized_len = strlen(normalized_uri);
    if (last_sep) {
        dir_len = (size_t)(last_sep - base_path + 1);
        if (dir_len > GLTF_MAX_URI_PATH_BYTES || normalized_len > GLTF_MAX_URI_PATH_BYTES ||
            dir_len > SIZE_MAX - normalized_len - 1u ||
            dir_len + normalized_len > GLTF_MAX_URI_PATH_BYTES) {
            free(normalized_uri);
            return NULL;
        }
        total_len = dir_len + normalized_len;
        resolved = (char *)malloc(total_len + 1u);
        if (!resolved) {
            free(normalized_uri);
            return NULL;
        }
        memcpy(resolved, base_path, dir_len);
        memcpy(resolved + dir_len, normalized_uri, normalized_len + 1u);
    } else {
        if (normalized_len > GLTF_MAX_URI_PATH_BYTES) {
            free(normalized_uri);
            return NULL;
        }
        resolved = normalized_uri;
        normalized_uri = NULL;
    }
    free(normalized_uri);
    return resolved;
}

/// @brief True if @p path uses the `asset://` scheme (a packed/mounted asset URI).
/// @param path Path string to classify.
/// @return Non-zero when @p path begins with the exact `asset://` prefix.
static int gltf_is_asset_uri(const char *path) {
    return path && strncmp(path, "asset://", 8) == 0;
}

/// @brief Read an entire file into a freshly malloc'd buffer (caller frees), capping at
///   256 MiB; returns NULL and zeroes @p out_size on any error.
/// @param filepath UTF-8 filesystem path to open.
/// @param out_size Optional destination for the complete byte count; cleared before reading.
/// @return Heap-allocated file bytes, or NULL for I/O, size, or allocation failure.
static uint8_t *gltf_read_file_bytes(const char *filepath, size_t *out_size) {
    if (out_size)
        *out_size = 0;
    if (!filepath)
        return NULL;
    FILE *f = rt_file_stdio_open_utf8(filepath, "rb");
    if (!f)
        return NULL;
    if (rt_file_stdio_seek64(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    int64_t fsize = rt_file_stdio_tell64(f);
    if (rt_file_stdio_seek64(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    if (fsize <= 0 || fsize > INT64_C(256) * 1024 * 1024) {
        fclose(f);
        return NULL;
    }
    uint8_t *file_data = (uint8_t *)malloc((size_t)fsize);
    if (!file_data) {
        fclose(f);
        return NULL;
    }
    if (fread(file_data, 1, (size_t)fsize, f) != (size_t)fsize) {
        free(file_data);
        fclose(f);
        return NULL;
    }
    fclose(f);
    if (out_size)
        *out_size = (size_t)fsize;
    return file_data;
}

/// @brief Load the root .gltf/.glb bytes: when @p load_assets, try the asset manager
///   first (and stop there for `asset://` URIs), otherwise fall back to the filesystem.
/// @param path Runtime string passed to the asset manager.
/// @param filepath UTF-8 path used for scheme checks and filesystem fallback.
/// @param load_assets Non-zero to consult the asset manager first.
/// @param out_size Optional destination for the loaded byte count.
/// @return Heap-allocated root bytes, or NULL when neither source can provide them.
static uint8_t *gltf_load_root_bytes(rt_string path,
                                     const char *filepath,
                                     int load_assets,
                                     size_t *out_size) {
    uint8_t *data = NULL;
    if (out_size)
        *out_size = 0;
    if (load_assets) {
        data = rt_asset_load_raw(path, out_size);
        if (data || gltf_is_asset_uri(filepath))
            return data;
    }
    return gltf_read_file_bytes(filepath, out_size);
}

/// @brief Load an external dependency's bytes (asset manager when @p load_assets, else
///   filesystem), failing if fewer than @p required_len bytes are available.
/// @param resource_path Resolved dependency key or filesystem path.
/// @param load_assets Non-zero to consult the asset manager before filesystem fallback.
/// @param required_len Minimum acceptable payload length.
/// @param preload_bundle Optional bundle from which staged bytes are consumed first.
/// @param out_size Optional destination for the complete loaded byte count.
/// @return Owned dependency bytes meeting @p required_len, or NULL on absence or failure.
static uint8_t *gltf_load_dependency_bytes(const char *resource_path,
                                           int load_assets,
                                           size_t required_len,
                                           rt_gltf_preload_bundle *preload_bundle,
                                           size_t *out_size) {
    uint8_t *data = NULL;
    size_t len = 0;
    if (out_size)
        *out_size = 0;
    if (!resource_path)
        return NULL;
    data = gltf_preload_bundle_take_dependency(
        preload_bundle, resource_path, GLTF_PRELOAD_DEP_BUFFER, &len);
    if (data)
        goto done;
    if (load_assets) {
        rt_string runtime_path = rt_const_cstr(resource_path);
        data = rt_asset_load_raw(runtime_path, &len);
        rt_string_unref(runtime_path);
        if (data || gltf_is_asset_uri(resource_path))
            goto done;
    }
    data = gltf_read_file_bytes(resource_path, &len);

done:
    if (!data)
        return NULL;
    if (len < required_len) {
        free(data);
        return NULL;
    }
    if (out_size)
        *out_size = len;
    return data;
}

/// @brief Take a preloaded image for @p key as a Pixels object, decoding from RGBA if needed.
/// @details Prefers an already-finished IMAGE_PIXELS dependency; otherwise consumes the staged
///          IMAGE_RGBA blob and builds Pixels from it. Returns NULL if neither is present.
/// @param preload_bundle Bundle containing decoded image dependencies.
/// @param key Exact dependency key to consume.
/// @return Transferred Pixels or source-preserving texture object, or NULL when unavailable.
static void *gltf_preload_take_decoded_image(rt_gltf_preload_bundle *preload_bundle,
                                             const char *key) {
    uint8_t *rgba_blob;
    uint8_t *source_data;
    size_t rgba_len = 0;
    size_t source_len = 0;
    void *pixels;
    pixels = gltf_preload_bundle_take_object_dependency(
        preload_bundle, key, GLTF_PRELOAD_DEP_IMAGE_PIXELS);
    if (!pixels) {
        rgba_blob = gltf_preload_bundle_take_dependency(
            preload_bundle, key, GLTF_PRELOAD_DEP_IMAGE_RGBA, &rgba_len);
        if (!rgba_blob)
            return NULL;
        pixels = gltf_pixels_from_rgba_blob(rgba_blob, rgba_len);
        free(rgba_blob);
    }
    source_data = gltf_preload_bundle_take_dependency(
        preload_bundle, key, GLTF_PRELOAD_DEP_IMAGE_SOURCE, &source_len);
    if (source_data) {
        pixels = gltf_preserve_decoded_image_source(pixels, source_data, source_len);
        free(source_data);
    }
    return pixels;
}

/// @brief Load and decode an external image dependency into a Pixels object: asset
///   manager (decoded by type) when @p load_assets, otherwise rt_pixels_load from disk.
/// @param resource_path Resolved image dependency key or filesystem path.
/// @param load_assets Non-zero to consult the asset manager before filesystem fallback.
/// @param preload_bundle Optional bundle supplying staged encoded or decoded image data.
/// @return Newly retained decoded Pixels or texture object, or NULL after all sources fail.
static void *gltf_load_dependency_image(const char *resource_path,
                                        int load_assets,
                                        rt_gltf_preload_bundle *preload_bundle) {
    uint8_t *staged = NULL;
    size_t staged_len = 0;
    int is_ktx2 = gltf_image_is_ktx2(resource_path);
    if (!resource_path || resource_path[0] == '\0')
        return NULL;
    {
        void *decoded = gltf_preload_take_decoded_image(preload_bundle, resource_path);
        if (decoded)
            return decoded;
    }
    staged = gltf_preload_bundle_take_dependency(
        preload_bundle, resource_path, GLTF_PRELOAD_DEP_IMAGE, &staged_len);
    if (staged) {
        void *decoded = is_ktx2 ? gltf_decode_ktx2_payload(staged, staged_len)
                                : rt_asset_decode_typed(resource_path, staged, staged_len);
        decoded = gltf_preserve_decoded_image_source(decoded, staged, staged_len);
        free(staged);
        if (decoded)
            return decoded;
    }
    if (load_assets) {
        size_t data_len = 0;
        rt_string runtime_path = rt_const_cstr(resource_path);
        uint8_t *data = rt_asset_load_raw(runtime_path, &data_len);
        rt_string_unref(runtime_path);
        if (data) {
            void *decoded = is_ktx2 ? gltf_decode_ktx2_payload(data, data_len)
                                    : rt_asset_decode_typed(resource_path, data, data_len);
            decoded = gltf_preserve_decoded_image_source(decoded, data, data_len);
            free(data);
            if (decoded)
                return decoded;
        }
        if (gltf_is_asset_uri(resource_path))
            return NULL;
    }
    if (is_ktx2) {
        size_t data_len = 0;
        uint8_t *data = gltf_read_file_bytes(resource_path, &data_len);
        void *decoded = data ? gltf_decode_ktx2_payload(data, data_len) : NULL;
        free(data);
        return decoded;
    }
    {
        size_t data_len = 0;
        uint8_t *data = gltf_read_file_bytes(resource_path, &data_len);
        if (data) {
            void *decoded = rt_asset_decode_typed(resource_path, data, data_len);
            decoded = gltf_preserve_decoded_image_source(decoded, data, data_len);
            free(data);
            if (decoded)
                return decoded;
        }
        {
            rt_string runtime_path = rt_const_cstr(resource_path);
            void *pixels = rt_pixels_load(runtime_path);
            rt_string_unref(runtime_path);
            return pixels;
        }
    }
}

/// @brief Write a NUL-terminated error message into the caller's buffer (no-op if buffer absent).
/// @param error Optional destination buffer.
/// @param error_cap Capacity of @p error including its terminator.
/// @param message Message to copy, or NULL for the default preload failure text.
static void gltf_preload_set_error(char *error, size_t error_cap, const char *message) {
    if (error && error_cap > 0) {
        snprintf(error, error_cap, "%s", message ? message : "failed to stage glTF preload");
    }
}
