//===----------------------------------------------------------------------===//
// Part of the Zanna project, under the GNU GPL v3. See LICENSE.
// File: src/runtime/graphics/3d/anim/rt_skeleton3d_skeleton.inc  (#included from rt_skeleton3d.c;
// not a standalone translation unit) Purpose: Skeleton3D bone hierarchy implementation
//===----------------------------------------------------------------------===//
/**
 * @file rt_skeleton3d_skeleton.inc
 * @brief Implements skeleton topology, bind poses, names, and retarget aliases.
 *
 * This implementation fragment owns bone insertion and retained exact-length
 * names, parent validation, cached bind-pose TRS values, model-space
 * inverse-bind generation, lookup and introspection, and the bounded legacy
 * alias table used during animation retargeting. It is included by
 * `rt_skeleton3d.c` and is not compiled independently.
 */
/*==========================================================================
 * Skeleton3D implementation
 *=========================================================================*/

/// @brief Release all valid retained bone names owned by a Skeleton3D.
/// @details The count is clamped through the private storage invariant before any
///          element is inspected. Registry validation protects finalization from a
///          corrupted private name slot while still balancing every published retain.
/// @param[in,out] skeleton Skeleton whose bone-name references are to be
/// released; may be `NULL`.
static void skeleton3d_release_bone_names(rt_skeleton3d *skeleton) {
    int32_t bone_count = skeleton3d_safe_bone_count(skeleton);
    for (int32_t i = 0; i < bone_count; ++i) {
        if (rt_string_is_handle(skeleton->bones[i].name))
            rt_string_unref(skeleton->bones[i].name);
        skeleton->bones[i].name = NULL;
    }
}

/// @brief Compare two runtime bone names without fixed buffers or C-string truncation.
/// @details Comparison uses the complete recorded byte lengths, so names that share a long
///          prefix remain distinct. NULL is treated as the empty string for defensive reads
///          of zero-initialized private slots.
/// @param[in] left First runtime name.
/// @param[in] right Second runtime name.
/// @return Nonzero exactly when both names contain the same byte sequence.
static int skeleton3d_names_equal(rt_string left, rt_string right) {
    int64_t left_len = left ? rt_str_len(left) : 0;
    int64_t right_len = right ? rt_str_len(right) : 0;
    const char *left_bytes;
    const char *right_bytes;
    if (left_len != right_len || left_len < 0)
        return 0;
    if (left_len == 0)
        return 1;
    left_bytes = rt_string_cstr(left);
    right_bytes = rt_string_cstr(right);
    return left_bytes && right_bytes && memcmp(left_bytes, right_bytes, (size_t)left_len) == 0;
}

/// @brief Compare one runtime bone name with a conventional NUL-terminated name.
/// @details Alias and persistence tables retain their historical C-string representation;
///          this bridge still compares the complete runtime-string length and never copies
///          into a fixed-size temporary buffer.
/// @param[in] runtime_name Length-carrying bone name; `NULL` represents empty.
/// @param[in] c_name Null-terminated comparison name; `NULL` represents empty.
/// @return Nonzero exactly when the two byte sequences match.
static int skeleton3d_name_equals_cstr(rt_string runtime_name, const char *c_name) {
    int64_t runtime_len = runtime_name ? rt_str_len(runtime_name) : 0;
    size_t c_len = c_name ? strlen(c_name) : 0;
    const char *runtime_bytes;
    if (runtime_len < 0 || (uint64_t)runtime_len != (uint64_t)c_len)
        return 0;
    if (c_len == 0)
        return 1;
    runtime_bytes = rt_string_cstr(runtime_name);
    return runtime_bytes && memcmp(runtime_bytes, c_name, c_len) == 0;
}

/// @brief Copy a runtime string into the legacy bounded alias-table representation.
/// @details Bone identity no longer uses this representation; it remains only for the
///          compatibility alias table whose public behavior historically caps each side at
///          63 bytes. The helper always terminates @p output and rejects invalid handles.
/// @param[in] name Runtime alias name; `NULL` produces an empty alias.
/// @param[out] output Destination buffer with room for 64 bytes.
/// @return Nonzero when the input was valid and the destination was initialized.
static int skeleton3d_copy_alias_name(rt_string name, char output[64]) {
    const char *bytes;
    size_t length;
    if (!output)
        return 0;
    output[0] = '\0';
    if (!name)
        return 1;
    if (!rt_string_is_handle(name))
        return 0;
    bytes = rt_string_cstr(name);
    if (!bytes)
        return 0;
    length = strlen(bytes);
    if (length > 63u)
        length = 63u;
    memcpy(output, bytes, length);
    output[length] = '\0';
    return 1;
}

/// @brief Clamp alias traversal to initialized backing storage.
/// @param skeleton Borrowed Skeleton3D payload.
/// @return Non-negative alias count bounded by the owned allocation.
static int32_t skeleton3d_safe_alias_count(const rt_skeleton3d *skeleton) {
    if (!skeleton || !skeleton->aliases || skeleton->alias_count <= 0 ||
        skeleton->alias_capacity <= 0)
        return 0;
    return skeleton->alias_count < skeleton->alias_capacity ? skeleton->alias_count
                                                            : skeleton->alias_capacity;
}

/// @brief GC finalizer for a Skeleton3D — releases every retained name and owned array.
/// @param[in,out] obj Skeleton payload being finalized.
/// @details Clears the bone and alias arrays and resets their logical counts
/// and capacities after releasing every valid bone-name reference.
static void rt_skeleton3d_finalize(void *obj) {
    rt_skeleton3d *s = (rt_skeleton3d *)obj;
    skeleton3d_release_bone_names(s);
    free(s->bones);
    s->bones = NULL;
    s->bone_count = 0;
    s->bone_capacity = 0;
    free(s->aliases);
    s->aliases = NULL;
    s->alias_count = 0;
    s->alias_capacity = 0;
}

/// @brief Allocate an empty Skeleton3D — no bones until `add_bone` is called.
/// @return A new mutable runtime skeleton, or `NULL` when allocation fails.
/// @details The object initially owns no arrays, contains no aliases, and
/// remains structurally mutable until a player, blender, or other pose consumer
/// freezes it.
void *rt_skeleton3d_new(void) {
    rt_skeleton3d *s =
        (rt_skeleton3d *)rt_obj_new_i64(RT_G3D_SKELETON3D_CLASS_ID, (int64_t)sizeof(rt_skeleton3d));
    if (!s) {
        rt_trap("Skeleton3D.New: memory allocation failed");
        return NULL;
    }
    s->vptr = NULL;
    s->bones = NULL;
    s->bone_count = 0;
    s->bone_capacity = 0;
    s->frozen = 0;
    s->aliases = NULL;
    s->alias_count = 0;
    s->alias_capacity = 0;
    rt_obj_set_finalizer(s, rt_skeleton3d_finalize);
    return s;
}

/// @brief Append one bone to the skeleton with its bind-pose matrix and parent reference.
///
/// `parent_index` of -1 marks a root bone. `bind_mat4` is the
/// parent-relative local transform in the rest pose; the model-space
/// inverse-bind matrix is computed lazily by
/// `rt_skeleton3d_compute_inverse_bind`.
///
/// @param[in,out] obj Mutable skeleton receiving the bone.
/// @param[in] name Exact runtime string retained for the bone; `NULL` selects
/// the shared empty string.
/// @param[in] parent_index `-1` for a root or the index of an already-added
/// parent.
/// @param[in] bind_mat4 Optional runtime Mat4 containing the row-major local
/// bind transform. Invalid or non-float-representable matrices become identity.
/// @return The newly-assigned bone index (0-based), or -1 on failure.
/// @details Bone insertion is rejected after the skeleton is frozen and at the
/// 1,024-bone ceiling. The local matrix is decomposed once to cache translation,
/// normalized rotation, and scale for animation fallback sampling.
int64_t rt_skeleton3d_add_bone(void *obj, rt_string name, int64_t parent_index, void *bind_mat4) {
    rt_skeleton3d *s = (rt_skeleton3d *)rt_g3d_checked_or_null(obj, RT_G3D_SKELETON3D_CLASS_ID);
    rt_string retained_name;
    if (!s)
        return -1;
    if (s->frozen) {
        rt_trap("Skeleton3D.AddBone: skeleton is already bound to an animation runtime");
        return -1;
    }
    s->bone_count = skeleton3d_safe_bone_count(s);
    if (!s->bones)
        s->bone_capacity = 0;
    if (s->bone_capacity < s->bone_count || s->bone_capacity > VGFX3D_MAX_SKELETON_BONES)
        s->bone_capacity = s->bone_count;
    if (s->bone_count >= VGFX3D_MAX_SKELETON_BONES) {
        rt_trap("Skeleton3D.AddBone: max 1024 bones exceeded");
        return -1;
    }
    // parent_index == -1 denotes a root bone. Any other negative value is
    // invalid — previously only the upper bound was checked, so callers could
    // pass -2, -100, etc. which survived validation and corrupted the
    // hierarchy in downstream code.
    if (parent_index < -1 || parent_index >= s->bone_count) {
        rt_trap("Skeleton3D.AddBone: parent_index must be -1 (root) or a valid bone index");
        return -1;
    }

    if (name && !rt_string_is_handle(name)) {
        rt_trap("Skeleton3D.AddBone: name must be a runtime string");
        return -1;
    }
    retained_name = rt_string_ref(name ? name : rt_str_empty());
    if (!retained_name)
        return -1;

    if (s->bone_count >= s->bone_capacity) {
        int32_t new_capacity = s->bone_capacity > 0 ? s->bone_capacity * 2 : 4;
        vgfx3d_bone_t *nb;
        if (new_capacity < s->bone_count + 1)
            new_capacity = s->bone_count + 1;
        if (new_capacity > VGFX3D_MAX_SKELETON_BONES)
            new_capacity = VGFX3D_MAX_SKELETON_BONES;
        nb = (vgfx3d_bone_t *)realloc(s->bones, (size_t)new_capacity * sizeof(vgfx3d_bone_t));
        if (!nb) {
            rt_string_unref(retained_name);
            return -1;
        }
        if (new_capacity > s->bone_capacity) {
            memset(
                nb + s->bone_capacity, 0, (size_t)(new_capacity - s->bone_capacity) * sizeof(*nb));
        }
        s->bones = nb;
        s->bone_capacity = new_capacity;
    }

    vgfx3d_bone_t *bone = &s->bones[s->bone_count];
    memset(bone, 0, sizeof(vgfx3d_bone_t));

    bone->name = retained_name;

    bone->parent_index = (int32_t)parent_index;

    /* Copy bind pose from Mat4 (double → float). Malformed matrices fall back to identity. */
    mat4_impl *bind = skeleton3d_mat4_checked(bind_mat4);
    if (bind) {
        int valid = 1;
        float local[16];
        for (int i = 0; i < 16; i++) {
            double value = bind->m[i];
            if (!skeleton3d_value_fits_float(value)) {
                valid = 0;
                break;
            }
            local[i] = (float)value;
        }
        if (valid)
            memcpy(bone->bind_pose_local, local, sizeof(local));
        else
            mat4f_identity(bone->bind_pose_local);
    } else {
        mat4f_identity(bone->bind_pose_local);
    }
    mat4f_identity(bone->inverse_bind);
    /* Cache the decomposed bind TRS once: sampling uses it as the per-channel
     * fallback every frame, and decomposition is the expensive part. */
    mat4f_decompose_trs(bone->bind_pose_local, bone->bind_pos, bone->bind_rot, bone->bind_scl);

    int64_t idx = s->bone_count;
    s->bone_count++;
    return idx;
}

/// @brief Precompute the inverse-bind matrix for every bone (model-space → bone-space).
///
/// Skinning multiplies each bone's animated model-space matrix by its
/// inverse-bind to displace each vertex from rest space into the
/// animated pose. Call this once after all bones are added.
///
/// @param[in,out] obj Skeleton whose inverse-bind matrices are regenerated.
/// @details Local bind matrices are composed through the hierarchy before
/// inversion. Allocation failure leaves existing inverse-bind values unchanged;
/// a singular or non-finite global bind matrix receives identity.
void rt_skeleton3d_compute_inverse_bind(void *obj) {
    rt_skeleton3d *s = (rt_skeleton3d *)rt_g3d_checked_or_null(obj, RT_G3D_SKELETON3D_CLASS_ID);
    float *bind_locals;
    float *globals;
    if (!s)
        return;
    s->bone_count = skeleton3d_safe_bone_count(s);
    if (s->bone_count <= 0)
        return;

    /* Heap-allocated: 1024-bone skeletons would need 128 KB of stack here. */
    bind_locals = (float *)malloc((size_t)s->bone_count * 16u * sizeof(float) * 2u);
    if (!bind_locals)
        return;
    globals = bind_locals + (size_t)s->bone_count * 16u;
    for (int32_t i = 0; i < s->bone_count; i++)
        memcpy(&bind_locals[i * 16], s->bones[i].bind_pose_local, 16 * sizeof(float));
    skeleton3d_compute_globals_from_locals(s, bind_locals, globals, s->bone_count);

    for (int32_t i = 0; i < s->bone_count; i++) {
        if (mat4f_invert(&globals[i * 16], s->bones[i].inverse_bind) != 0)
            mat4f_identity(s->bones[i].inverse_bind);
    }
    free(bind_locals);
}

/// @brief Number of bones in the skeleton (0 for NULL).
/// @param[in,out] obj Skeleton to query.
/// @return The readable bone count after clamping corrupt metadata to the
/// allocation and skeleton ceiling, or zero for an invalid object.
int64_t rt_skeleton3d_get_bone_count(void *obj) {
    rt_skeleton3d *s = (rt_skeleton3d *)rt_g3d_checked_or_null(obj, RT_G3D_SKELETON3D_CLASS_ID);
    if (!s)
        return 0;
    s->bone_count = skeleton3d_safe_bone_count(s);
    return s->bone_count;
}

/// @brief Linear exact-length search for a bone by name; returns its index or -1 if absent.
/// @param[in,out] obj Skeleton to search.
/// @param[in] name Valid runtime string matched by complete byte length and
/// content.
/// @return The first matching zero-based index, or `-1` for invalid input or
/// no match.
int64_t rt_skeleton3d_find_bone(void *obj, rt_string name) {
    rt_skeleton3d *s = (rt_skeleton3d *)rt_g3d_checked_or_null(obj, RT_G3D_SKELETON3D_CLASS_ID);
    if (!s || !name)
        return -1;
    s->bone_count = skeleton3d_safe_bone_count(s);
    if (!rt_string_is_handle(name))
        return -1;
    for (int32_t i = 0; i < s->bone_count; i++)
        if (skeleton3d_names_equal(s->bones[i].name, name))
            return i;
    return -1;
}

/// @brief Search for a bone by name and wrap the optional index in an Option.
/// @details This preserves `rt_skeleton3d_find_bone` exactly, but turns the
///          legacy `-1` sentinel into `None` for callers that want explicit
///          absence handling.
/// @param[in,out] obj Skeleton3D handle.
/// @param[in] name Exact bone name to search for.
/// @return `Some(index)` for a matching bone, otherwise `None`.
void *rt_skeleton3d_find_bone_option(void *obj, rt_string name) {
    int64_t index = rt_skeleton3d_find_bone(obj, name);
    return index >= 0 ? rt_option_some_i64(index) : rt_option_none();
}

/// @brief Read the exact name of bone at `index` as a retained runtime-string reference.
/// @details The caller owns the returned reference. Invalid skeletons and indexes return
///          the shared empty string, preserving the historical non-NULL result contract.
/// @param[in,out] obj Skeleton to query.
/// @param[in] index Zero-based bone index.
/// @return A retained exact bone-name string, or a runtime empty string when
/// the object or index is invalid.
rt_string rt_skeleton3d_get_bone_name(void *obj, int64_t index) {
    rt_skeleton3d *s = (rt_skeleton3d *)rt_g3d_checked_or_null(obj, RT_G3D_SKELETON3D_CLASS_ID);
    if (!s)
        return rt_const_cstr("");
    s->bone_count = skeleton3d_safe_bone_count(s);
    if (index < 0 || index >= s->bone_count)
        return rt_const_cstr("");
    return rt_string_ref(s->bones[index].name ? s->bones[index].name : rt_str_empty());
}

/// @brief Internal: parent index of the bone at @p index (-1 for roots/out of range).
/// @param[in,out] obj Skeleton to query.
/// @param[in] index Zero-based bone index.
/// @return A validated parent index, or `-1` for a root, invalid relation,
/// invalid skeleton, or out-of-range bone.
int64_t rt_skeleton3d_get_bone_parent_raw(void *obj, int64_t index) {
    rt_skeleton3d *s = (rt_skeleton3d *)rt_g3d_checked_or_null(obj, RT_G3D_SKELETON3D_CLASS_ID);
    if (!s)
        return -1;
    s->bone_count = skeleton3d_safe_bone_count(s);
    if (index < 0 || index >= s->bone_count)
        return -1;
    return skeleton3d_valid_parent_index(s, (int32_t)index, s->bone_count);
}

/// @brief Internal: copy the bone's parent-relative bind matrix (row-major 16
///        doubles) into @p out16; returns 0 on out-of-range.
/// @param[in,out] obj Skeleton to query.
/// @param[in] index Zero-based bone index.
/// @param[out] out16 Destination with room for 16 doubles.
/// @return One when a valid matrix was copied, otherwise zero.
int8_t rt_skeleton3d_get_bone_bind_local_raw(void *obj, int64_t index, double *out16) {
    rt_skeleton3d *s = (rt_skeleton3d *)rt_g3d_checked_or_null(obj, RT_G3D_SKELETON3D_CLASS_ID);
    if (!s || !out16)
        return 0;
    s->bone_count = skeleton3d_safe_bone_count(s);
    if (index < 0 || index >= s->bone_count)
        return 0;
    const float *m = s->bones[index].bind_pose_local;
    for (int i = 0; i < 16; ++i)
        out16[i] = (double)m[i];
    return 1;
}

/// @brief Copy one bone's parent-relative bind transform into a runtime Mat4.
/// @param[in,out] obj Skeleton to query.
/// @param[in] index Zero-based bone index.
/// @return A newly allocated row-major Mat4, or `NULL` for an invalid skeleton
/// or out-of-range index.
void *rt_skeleton3d_get_bone_bind_pose(void *obj, int64_t index) {
    rt_skeleton3d *s = (rt_skeleton3d *)rt_g3d_checked_or_null(obj, RT_G3D_SKELETON3D_CLASS_ID);
    if (!s)
        return NULL;
    s->bone_count = skeleton3d_safe_bone_count(s);
    if (index < 0 || index >= s->bone_count)
        return NULL;
    const float *m = s->bones[index].bind_pose_local;
    return rt_mat4_new(m[0],
                       m[1],
                       m[2],
                       m[3],
                       m[4],
                       m[5],
                       m[6],
                       m[7],
                       m[8],
                       m[9],
                       m[10],
                       m[11],
                       m[12],
                       m[13],
                       m[14],
                       m[15]);
}

/// @brief Register (or remove) an external bone-name alias for retargeting.
///
/// `external` is the name a foreign rig uses; `local` is this skeleton's bone
/// it should map to. Retargeting consults these aliases before falling back
/// to humanoid-role inference — the deterministic escape hatch for arbitrary
/// downloaded rigs (e.g. `mixamorig:Hips` -> `hips`). Re-registering an
/// external name replaces the mapping; an empty/NULL `local` removes it.
/// The alias table is not consumed by skinning or pose math, so it may be
/// edited freely even on a frozen skeleton.
///
/// @param[in,out] obj Skeleton whose alias table is modified.
/// @param[in] external Nonempty foreign-rig name, truncated to 63 bytes.
/// @param[in] local Local bone name, also truncated to 63 bytes; `NULL` or
/// empty removes an existing mapping.
/// @details Matching is exact and case-sensitive after truncation. Replacement
/// preserves the table slot, while removal fills the gap with the last entry
/// and therefore does not preserve alias order.
void rt_skeleton3d_set_bone_alias(void *obj, rt_string external, rt_string local) {
    rt_skeleton3d *s = (rt_skeleton3d *)rt_g3d_checked_or_null(obj, RT_G3D_SKELETON3D_CLASS_ID);
    char ext_buf[64];
    char local_buf[64];
    if (!s)
        return;
    if (!skeleton3d_copy_alias_name(external, ext_buf) || ext_buf[0] == '\0') {
        rt_trap("Skeleton3D.SetBoneAlias: external name must be non-empty");
        return;
    }
    if (!skeleton3d_copy_alias_name(local, local_buf))
        local_buf[0] = '\0';

    s->alias_count = skeleton3d_safe_alias_count(s);
    if (!s->aliases)
        s->alias_capacity = 0;
    if (s->alias_capacity < s->alias_count)
        s->alias_capacity = s->alias_count;

    int32_t found = -1;
    for (int32_t i = 0; i < s->alias_count; i++) {
        if (strcmp(s->aliases[i].external, ext_buf) == 0) {
            found = i;
            break;
        }
    }
    if (local_buf[0] == '\0') {
        /* Removal. */
        if (found >= 0) {
            s->aliases[found] = s->aliases[s->alias_count - 1];
            s->alias_count--;
        }
        return;
    }
    if (found < 0) {
        if (s->alias_count >= s->alias_capacity) {
            int32_t cap;
            if (s->alias_count == INT32_MAX)
                return;
            if (s->alias_capacity <= 0)
                cap = 8;
            else if (s->alias_capacity > INT32_MAX / 2)
                cap = s->alias_count + 1;
            else
                cap = s->alias_capacity * 2;
            if (cap <= s->alias_count || (size_t)cap > SIZE_MAX / sizeof(*s->aliases))
                return;
            rt_skeleton3d_alias *grown =
                (rt_skeleton3d_alias *)realloc(s->aliases, (size_t)cap * sizeof(*grown));
            if (!grown)
                return;
            s->aliases = grown;
            s->alias_capacity = cap;
        }
        found = s->alias_count++;
    }
    memcpy(s->aliases[found].external, ext_buf, sizeof(ext_buf));
    memcpy(s->aliases[found].local, local_buf, sizeof(local_buf));
}

/// @brief Number of registered bone-name aliases.
/// @param[in] obj Skeleton to query.
/// @return The stored alias count, or zero for an invalid object.
int64_t rt_skeleton3d_get_alias_count(void *obj) {
    rt_skeleton3d *s = (rt_skeleton3d *)rt_g3d_checked_or_null(obj, RT_G3D_SKELETON3D_CLASS_ID);
    return skeleton3d_safe_alias_count(s);
}

/// @brief Resolve an external bone name through the alias table.
/// @param[in] s Skeleton whose aliases are searched.
/// @param[in] external Nonempty null-terminated foreign-rig name.
/// @return The mapped local bone name, or NULL when no alias matches.
/// @note The returned pointer is borrowed from the skeleton and becomes invalid
/// if that alias is replaced or removed, the table grows, or the skeleton is
/// destroyed.
const char *skeleton3d_alias_lookup(const rt_skeleton3d *s, const char *external) {
    if (!s || !external || !external[0])
        return NULL;
    for (int32_t i = 0, count = skeleton3d_safe_alias_count(s); i < count; i++) {
        if (strcmp(s->aliases[i].external, external) == 0)
            return s->aliases[i].local;
    }
    return NULL;
}
