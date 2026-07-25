//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/render/rt_model3d_core.inc
// Purpose: Model3D types, lifecycle, scene-tree cloning, resource collection,
//   and default animation binding.
// Key invariants:
//   - Model component arrays contain retained, class-valid resource handles.
//   - Scene-tree collection deduplicates shared meshes, materials, and skeletons.
//   - Mutable instances own deep copies of bounded typed node metadata.
// Ownership/Lifetime:
//   - This implementation fragment is included by rt_model3d.c.
//   - Model3D finalization releases every retained component and scene reference.
// Links: rt_model3d.c, rt_model3d.h, rt_scene3d_internal.h,
//   rt_canvas3d_internal.h,
//   docs/adr/0159-typed-scenenode-metadata-and-vscn-v6.md
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements Model3D storage, lifetime, scene cloning, and resource collection.
/// @details This include fragment is compiled as part of `rt_model3d.c`. It repairs private
///          count/capacity state before access, retains deduplicated imported resources, and
///          deep-copies mutable node and metadata state for each model instance.

typedef struct {
    rt_scene_node3d *root;
    char *name;
    void **cameras;
    int32_t camera_count;
    int32_t camera_capacity;
} model3d_scene_entry;

typedef struct {
    void *vptr;
    rt_scene_node3d *template_root;

    void **meshes;
    int32_t mesh_count;
    int32_t mesh_capacity;

    void **materials;
    int32_t material_count;
    int32_t material_capacity;

    void **skeletons;
    int32_t skeleton_count;
    int32_t skeleton_capacity;

    void **animations;
    int32_t animation_count;
    int32_t animation_capacity;

    void **node_animations;
    int32_t node_animation_count;
    int32_t node_animation_capacity;

    void **cameras;
    int32_t camera_count;
    int32_t camera_capacity;

    model3d_scene_entry *scenes;
    int32_t scene_count;
    int32_t scene_capacity;

    /* KHR_materials_variants: variant display names copied from the importer.
     * The per-node material tables live on the scene nodes themselves. */
    char **variant_names;
    int32_t variant_count;
} rt_model3d;

/// @brief Validate @p obj as a Model3D handle and return its typed pointer (NULL on mismatch).
/// @param obj Candidate runtime object handle.
/// @return Borrowed Model3D pointer, or NULL for invalid input.
static rt_model3d *model3d_checked(void *obj) {
    return (rt_model3d *)rt_g3d_checked_or_null(obj, RT_G3D_MODEL3D_CLASS_ID);
}

/// @brief Clamp a runtime count to its backing capacity and pointer validity.
/// @param arr Backing array pointer.
/// @param count Stored logical count.
/// @param capacity Stored allocation capacity.
/// @return Safe non-negative count no greater than capacity, or zero without valid storage.
static int32_t model_clamped_array_count(const void *arr, int32_t count, int32_t capacity) {
    if (count <= 0)
        return 0;
    if (!arr || capacity <= 0)
        return 0;
    return count > capacity ? capacity : count;
}

/// @brief Clamp and persist a private array count before mutating that array.
/// @param arr Backing array pointer.
/// @param count Address of the stored count to repair.
/// @param capacity Stored allocation capacity.
/// @return Repaired safe count.
static int32_t model_repair_array_count(const void *arr, int32_t *count, int32_t capacity) {
    int32_t safe_count = count ? model_clamped_array_count(arr, *count, capacity) : 0;
    if (count)
        *count = safe_count;
    return safe_count;
}

/// @brief Return a safe child count for scene-node walkers.
/// @param node Scene node whose child array is inspected.
/// @return Safe child count, or zero for invalid storage.
static int32_t model_node_child_count(const rt_scene_node3d *node) {
    return node ? model_clamped_array_count(node->children, node->child_count, node->child_capacity)
                : 0;
}

/// @brief Return a safe LOD count for scene-node cloning/ref collection.
/// @param node Scene node whose LOD array is inspected.
/// @return Safe LOD-entry count, or zero for invalid storage.
static int32_t model_node_lod_count(const rt_scene_node3d *node) {
    return node ? model_clamped_array_count(node->lod_levels, node->lod_count, node->lod_capacity)
                : 0;
}

/// @brief Return a safe scene count for public scene accessors.
/// @param model Model3D whose scene table is inspected.
/// @return Safe scene-entry count, or zero for invalid storage.
static int32_t model_scene_count(const rt_model3d *model) {
    return model
               ? model_clamped_array_count(model->scenes, model->scene_count, model->scene_capacity)
               : 0;
}

/// @brief Clamp and persist the scene count before mutating the scene table.
/// @param model Model3D whose stored scene count is repaired.
/// @return Repaired safe scene count.
static int32_t model_repair_scene_count(rt_model3d *model) {
    int32_t safe_count = model_scene_count(model);
    if (model)
        model->scene_count = safe_count;
    return safe_count;
}

/// @brief Return a C string only when @p string is a valid runtime string handle.
/// @param string Candidate runtime string.
/// @return Borrowed NUL-terminated bytes, or NULL for an invalid handle.
static const char *model_string_cstr_or_null(rt_string string) {
    if (!string || !rt_string_is_handle(string))
        return NULL;
    return rt_string_cstr(string);
}

/// @brief Convert an importer-reported int64 count into a runtime array count.
/// @param count Imported count.
/// @param out Receives the non-negative int32 count.
/// @param trap_msg Optional trap message used when @p count exceeds `INT32_MAX`.
/// @return One on successful conversion, or zero for invalid output or overflow.
static int model_count_i64_to_i32(int64_t count, int32_t *out, const char *trap_msg) {
    if (!out)
        return 0;
    if (count <= 0) {
        *out = 0;
        return 1;
    }
    if (count > INT32_MAX) {
        rt_trap(trap_msg ? trap_msg : "Model3D.Load: imported count is too large");
        *out = 0;
        return 0;
    }
    *out = (int32_t)count;
    return 1;
}

/// @brief Release the GC reference at `*slot` and clear the slot. NULL-safe both ways
/// (`slot == NULL` or `*slot == NULL`). Frees the object only when the release drops the
/// retain count to zero — bystander references stay alive.
/// @param slot Address of the owned reference to release and clear.
static void model_release_ref(void **slot) {
    rt_g3d_ref_slot_release(slot);
}

/// @brief Release an owned Graphics3D slot only when it still has the expected class.
/// @details Model3D assignment paths validate classes before retaining. If private state is later
///          corrupted to an unrelated object, clear it without releasing because the runtime cannot
///          prove the mismatched handle is owned by this slot.
/// @param slot Address of the owned Graphics3D reference.
/// @param class_id Expected runtime class identifier.
static void model_release_class_ref(void **slot, int64_t class_id) {
    if (!slot || !*slot)
        return;
    if (!rt_g3d_has_class(*slot, class_id)) {
        rt_g3d_ref_slot_clear_unowned(slot);
        return;
    }
    model_release_ref(slot);
}

/// @brief Release a temporary local reference (e.g. a freshly-cloned scene-node we just
/// re-parented and no longer need). Wrapper around `model_release_ref` that takes a value
/// rather than a slot, so call sites can write `model_release_local(node)` instead of
/// passing `&node` and worrying about the post-release zero.
/// @param obj Nullable temporary owned reference to release.
static void model_release_local(void *obj) {
    void *tmp = obj;
    model_release_ref(&tmp);
}

/// @brief Release every entry in an array whose elements are expected to share one class.
/// @param arr Address of the owned array pointer, freed and cleared.
/// @param count Address of the logical count, reset to zero.
/// @param capacity Address of the capacity, reset to zero.
/// @param class_id Expected class for owned entries.
static void model_release_array_typed(void ***arr,
                                      int32_t *count,
                                      int32_t *capacity,
                                      int64_t class_id) {
    int32_t release_count;
    if (!arr)
        return;
    if (!*arr) {
        if (count)
            *count = 0;
        if (capacity)
            *capacity = 0;
        return;
    }
    release_count = model_clamped_array_count(*arr, count ? *count : 0, capacity ? *capacity : 0);
    if (count) {
        for (int32_t i = 0; i < release_count; i++)
            model_release_class_ref(&(*arr)[i], class_id);
        *count = 0;
    }
    if (capacity)
        *capacity = 0;
    free(*arr);
    *arr = NULL;
}

/// @brief Release every scene entry's root, name, and camera list (does not free the array itself).
/// @param model Model3D whose scene table and entries are released.
static void model_release_scenes(rt_model3d *model) {
    if (!model)
        return;
    if (!model->scenes) {
        model->scene_count = 0;
        model->scene_capacity = 0;
        return;
    }
    for (int32_t i = 0, scene_count = model_scene_count(model); i < scene_count; i++) {
        model3d_scene_entry *scene = &model->scenes[i];
        model_release_class_ref((void **)&scene->root, RT_G3D_SCENENODE3D_CLASS_ID);
        free(scene->name);
        scene->name = NULL;
        model_release_array_typed(&scene->cameras,
                                  &scene->camera_count,
                                  &scene->camera_capacity,
                                  RT_G3D_CAMERA3D_CLASS_ID);
    }
    free(model->scenes);
    model->scenes = NULL;
    model->scene_count = 0;
    model->scene_capacity = 0;
}

/// @brief GC finalizer for `Model3D`. Drops references on the template scene-graph root,
/// then walks each component list (meshes, materials, skeletons, animations) and releases
/// every entry. Underlying assets that other live owners still reference remain alive;
/// solo-owned assets get freed.
/// @param obj Model3D instance being finalized.
static void rt_model3d_finalize(void *obj) {
    rt_model3d *model = (rt_model3d *)obj;
    if (!model)
        return;
    model_release_class_ref((void **)&model->template_root, RT_G3D_SCENENODE3D_CLASS_ID);
    model_release_scenes(model);
    model_release_array_typed(
        &model->meshes, &model->mesh_count, &model->mesh_capacity, RT_G3D_MESH3D_CLASS_ID);
    model_release_array_typed(&model->materials,
                              &model->material_count,
                              &model->material_capacity,
                              RT_G3D_MATERIAL3D_CLASS_ID);
    model_release_array_typed(&model->skeletons,
                              &model->skeleton_count,
                              &model->skeleton_capacity,
                              RT_G3D_SKELETON3D_CLASS_ID);
    model_release_array_typed(&model->animations,
                              &model->animation_count,
                              &model->animation_capacity,
                              RT_G3D_ANIMATION3D_CLASS_ID);
    model_release_array_typed(&model->node_animations,
                              &model->node_animation_count,
                              &model->node_animation_capacity,
                              RT_G3D_NODEANIMATION3D_CLASS_ID);
    model_release_array_typed(
        &model->cameras, &model->camera_count, &model->camera_capacity, RT_G3D_CAMERA3D_CLASS_ID);
    if (model->variant_names) {
        for (int32_t i = 0; i < model->variant_count; i++)
            free(model->variant_names[i]);
        free(model->variant_names);
    }
    model->variant_names = NULL;
    model->variant_count = 0;
}

/// @brief Allocate a zeroed `rt_model3d` with finalizer wired and a fresh template-root
/// scene node attached. Traps with a user-visible message on either allocation failure;
/// returns NULL only after trapping so the caller's `goto fail` paths can still run.
/// @return New zeroed Model3D with an owned template root, or NULL on allocation failure.
static rt_model3d *model_new(void) {
    rt_model3d *model =
        (rt_model3d *)rt_obj_new_i64(RT_G3D_MODEL3D_CLASS_ID, (int64_t)sizeof(rt_model3d));
    if (!model) {
        rt_trap("Model3D.Load: allocation failed");
        return NULL;
    }
    memset(model, 0, sizeof(*model));
    rt_obj_set_finalizer(model, rt_model3d_finalize);
    model->template_root = (rt_scene_node3d *)rt_scene_node3d_new();
    if (!model->template_root) {
        rt_trap("Model3D.Load: template root allocation failed");
        model_release_local(model);
        return NULL;
    }
    return model;
}

/// @brief Ensure `*arr` can hold at least `need` entries, doubling capacity (initial 4)
/// when it grows. Returns 1 on success, 0 on realloc failure (existing array untouched so
/// the caller can safely trap and bail).
/// @param arr Address of the owned pointer array.
/// @param cap Address of its capacity.
/// @param need Minimum required entry capacity.
/// @return One when capacity is available, or zero on invalid input, overflow, or allocation failure.
static int model_grow_array(void ***arr, int32_t *cap, int32_t need) {
    int32_t new_cap;
    int32_t old_cap;
    if (need < 0 || !cap || !arr)
        return 0;
    if (*cap < 0)
        *cap = 0;
    if (*cap >= need && *arr)
        return 1;
    if (!*arr)
        *cap = 0;
    new_cap = *cap > 0 ? *cap : 4;
    while (new_cap < need) {
        if (new_cap > INT32_MAX / 2) {
            new_cap = need;
            break;
        }
        new_cap *= 2;
    }
    if (new_cap < need)
        return 0;
    if ((size_t)new_cap > SIZE_MAX / sizeof(void *))
        return 0;
    old_cap = *cap;
    void **grown = (void **)realloc(*arr, (size_t)new_cap * sizeof(void *));
    if (!grown)
        return 0;
    memset(grown + old_cap, 0, (size_t)(new_cap - old_cap) * sizeof(void *));
    *arr = grown;
    *cap = new_cap;
    return 1;
}

static int model_append_ref(
    void ***arr, int32_t *count, int32_t *cap, void *obj, const char *trap_msg);

/// @brief Duplicate @p value, or @p fallback when @p value is NULL/empty (traps on alloc failure).
/// @param value Preferred nullable source string.
/// @param fallback Fallback source used for NULL or empty @p value.
/// @return Newly allocated copy, or NULL after trapping on allocation failure.
static char *model_strdup_or(const char *value, const char *fallback) {
    const char *src = (value && value[0] != '\0') ? value : fallback;
    size_t len;
    char *copy;
    if (!src)
        src = "";
    len = strlen(src);
    if (len == SIZE_MAX) {
        rt_trap("Model3D.Load: scene name too long");
        return NULL;
    }
    copy = (char *)malloc(len + 1);
    if (!copy) {
        rt_trap("Model3D.Load: scene name allocation failed");
        return NULL;
    }
    memcpy(copy, src, len + 1);
    return copy;
}

/// @brief Ensure the model's scene array holds at least @p need entries (zero-filled growth).
/// @param model Model3D whose scene table may grow.
/// @param need Minimum required entry capacity.
/// @return 1 on success, 0 on overflow or reallocation failure.
static int model_grow_scenes(rt_model3d *model, int32_t need) {
    model3d_scene_entry *grown;
    int32_t old_capacity;
    int32_t new_cap;
    if (!model || need < 0)
        return 0;
    if (model->scene_capacity < 0)
        model->scene_capacity = 0;
    if (model->scene_capacity >= need && model->scenes)
        return 1;
    if (!model->scenes)
        model->scene_capacity = 0;
    old_capacity = model->scene_capacity;
    new_cap = old_capacity > 0 ? old_capacity : 4;
    while (new_cap < need) {
        if (new_cap > INT32_MAX / 2) {
            new_cap = need;
            break;
        }
        new_cap *= 2;
    }
    if (new_cap < need)
        return 0;
    if ((size_t)new_cap > SIZE_MAX / sizeof(*model->scenes))
        return 0;
    grown = (model3d_scene_entry *)realloc(model->scenes, (size_t)new_cap * sizeof(*grown));
    if (!grown)
        return 0;
    memset(grown + old_capacity, 0, (size_t)(new_cap - old_capacity) * sizeof(*grown));
    model->scenes = grown;
    model->scene_capacity = new_cap;
    return 1;
}

/// @brief Append a scene (retained @p root + name) to the model, synthesizing a name if empty.
/// @details Uses "default" for the first scene, else "scene_N". Reports the new index via
///          @p out_index. Returns 0 and traps on growth or name-allocation failure.
/// @param model Destination Model3D.
/// @param name Nullable authored scene name.
/// @param root Valid SceneNode3D root retained by the entry.
/// @param out_index Optional destination for the new scene index.
/// @return One on success, or zero for invalid input or allocation failure.
static int model_append_scene_entry(rt_model3d *model,
                                    const char *name,
                                    rt_scene_node3d *root,
                                    int32_t *out_index) {
    model3d_scene_entry *scene;
    char fallback[64];
    int32_t scene_count;
    if (!model || !rt_g3d_has_class(root, RT_G3D_SCENENODE3D_CLASS_ID))
        return 0;
    scene_count = model_repair_scene_count(model);
    if (scene_count >= INT32_MAX || !model_grow_scenes(model, scene_count + 1)) {
        rt_trap("Model3D.Load: scene list allocation failed");
        return 0;
    }
    snprintf(
        fallback, sizeof(fallback), scene_count == 0 ? "default" : "scene_%d", (int)scene_count);
    scene = &model->scenes[scene_count];
    memset(scene, 0, sizeof(*scene));
    rt_obj_retain_maybe(root);
    scene->root = root;
    scene->name = model_strdup_or(name, fallback);
    if (!scene->name) {
        model_release_ref((void **)&scene->root);
        memset(scene, 0, sizeof(*scene));
        return 0;
    }
    if (out_index)
        *out_index = scene_count;
    model->scene_count = scene_count + 1;
    return 1;
}

/// @brief Append a Camera3D to a scene entry's camera list (no-op for NULL args).
/// @param scene Destination scene entry.
/// @param camera Candidate Camera3D; NULL and wrong-class objects are ignored.
/// @return One for a successful append or ignored input, or zero on allocation failure.
static int model_append_scene_camera(model3d_scene_entry *scene, void *camera) {
    if (!scene || !camera)
        return 1;
    if (!rt_g3d_has_class(camera, RT_G3D_CAMERA3D_CLASS_ID))
        return 1;
    return model_append_ref(&scene->cameras,
                            &scene->camera_count,
                            &scene->camera_capacity,
                            camera,
                            "Model3D.Load: scene camera list allocation failed");
}

/// @brief Append a retained reference to `obj` at the tail of `*arr`. Grows storage if
/// needed; traps with `trap_msg` and returns 0 on allocation failure. Always retains `obj`
/// (NULL-safe via `rt_obj_retain_maybe`) so the model owns a stable count for the lifetime
/// of the entry.
/// @param arr Address of the retained-reference array.
/// @param count Address of the logical count.
/// @param cap Address of the allocation capacity.
/// @param obj Nullable object to append and retain.
/// @param trap_msg Allocation failure trap message.
/// @return One on append or NULL no-op, or zero on invalid state or allocation failure.
static int model_append_ref(
    void ***arr, int32_t *count, int32_t *cap, void *obj, const char *trap_msg) {
    int32_t safe_count;
    if (!obj)
        return 1;
    if (!arr || !count || !cap)
        return 0;
    safe_count = model_repair_array_count(*arr, count, *cap);
    if (safe_count >= INT32_MAX)
        return 0;
    if (!model_grow_array(arr, cap, safe_count + 1)) {
        rt_trap(trap_msg);
        return 0;
    }
    rt_obj_retain_maybe(obj);
    (*arr)[safe_count] = obj;
    *count = safe_count + 1;
    return 1;
}

/// @brief Append `obj` only if the array does not already contain that exact pointer.
/// Used for collecting unique mesh / material references off a scene-graph walk where the
/// same asset may be referenced from many nodes. NULL `obj` is treated as success-noop.
/// @param arr Address of the retained-reference array.
/// @param count Address of the logical count.
/// @param cap Address of the allocation capacity.
/// @param obj Nullable object to deduplicate and retain.
/// @param trap_msg Allocation failure trap message.
/// @return One when already present, appended, or NULL; zero on invalid state or failure.
static int model_append_unique_ref(
    void ***arr, int32_t *count, int32_t *cap, void *obj, const char *trap_msg) {
    int32_t safe_count;
    if (!obj)
        return 1;
    if (!arr || !count || !cap)
        return 0;
    safe_count = model_repair_array_count(*arr, count, *cap);
    for (int32_t i = 0; i < safe_count; i++) {
        if ((*arr)[i] == obj)
            return 1;
    }
    return model_append_ref(arr, count, cap, obj, trap_msg);
}

/// @brief Register one class-valid mesh and its attached skeleton in the model inventory.
/// @details Both references are deduplicated independently because a scene may reuse a mesh or
///          attach one skeleton to several meshes and LOD levels. Invalid mesh and skeleton
///          handles are ignored so a corrupt private scene node cannot poison public accessors.
/// @param model Destination Model3D inventory.
/// @param candidate Candidate Mesh3D handle.
/// @return One when valid references are collected or ignored; zero on allocation failure.
static int model_collect_mesh_refs(rt_model3d *model, void *candidate) {
    rt_mesh3d *mesh = (rt_mesh3d *)rt_g3d_checked_or_null(candidate, RT_G3D_MESH3D_CLASS_ID);
    void *skeleton =
        mesh ? rt_g3d_checked_or_null(mesh->skeleton_ref, RT_G3D_SKELETON3D_CLASS_ID) : NULL;
    if (!model_append_unique_ref(&model->meshes,
                                 &model->mesh_count,
                                 &model->mesh_capacity,
                                 mesh,
                                 "Model3D.Load: mesh list allocation failed"))
        return 0;
    return model_append_unique_ref(&model->skeletons,
                                   &model->skeleton_count,
                                   &model->skeleton_capacity,
                                   skeleton,
                                   "Model3D.Load: skeleton list allocation failed");
}

/// @brief Recursively count nodes in the subtree rooted at `node`, including `node`
/// itself. Used by `rt_model3d_get_node_count` (which subtracts 1 to exclude the synthetic
/// template root from the user-visible count).
/// @param node Subtree root.
/// @return Node count including @p node, or zero for NULL, overflow, or traversal allocation failure.
static int32_t model_count_subtree(const rt_scene_node3d *node) {
    const rt_scene_node3d **stack = NULL;
    int32_t stack_count = 0;
    int32_t stack_capacity = 0;
    int32_t total = 0;
    if (!node)
        return 0;
    stack_capacity = 32;
    stack = (const rt_scene_node3d **)malloc((size_t)stack_capacity * sizeof(rt_scene_node3d *));
    if (!stack) {
        rt_trap("Model3D: node-count stack allocation failed");
        return 0;
    }
    stack[stack_count++] = node;
    while (stack_count > 0) {
        const rt_scene_node3d *current = stack[--stack_count];
        if (!current)
            continue;
        if (total >= MODEL3D_MAX_WALK_NODES || total == INT32_MAX) {
            free(stack);
            rt_trap("Model3D: too many nodes");
            return 0;
        }
        total++;
        int32_t child_count = model_node_child_count(current);
        for (int32_t i = 0; i < child_count; i++) {
            if (stack_count >= stack_capacity) {
                if (stack_capacity > INT32_MAX / 2 ||
                    (size_t)(stack_capacity * 2) > SIZE_MAX / sizeof(rt_scene_node3d *)) {
                    free(stack);
                    rt_trap("Model3D: too many nodes");
                    return 0;
                }
                int32_t new_capacity = stack_capacity * 2;
                const rt_scene_node3d **grown = (const rt_scene_node3d **)realloc(
                    stack, (size_t)new_capacity * sizeof(rt_scene_node3d *));
                if (!grown) {
                    free(stack);
                    rt_trap("Model3D: node-count stack allocation failed");
                    return 0;
                }
                stack = grown;
                stack_capacity = new_capacity;
            }
            const rt_scene_node3d *child = (const rt_scene_node3d *)rt_g3d_checked_or_null(
                current->children[i], RT_G3D_SCENENODE3D_CLASS_ID);
            if (child)
                stack[stack_count++] = child;
        }
    }
    free(stack);
    return total;
}

/// @brief Derive a friendly name for the template root from the source asset's file path
/// — strips the directory prefix (handles both `/` and `\` separators so a Windows-authored
/// path works on macOS/Linux) and the file extension. Falls back to a no-op when the
/// resulting name would be empty. Truncates at 127 bytes to fit the local stack buffer.
/// @param root Template root whose name is updated.
/// @param path_cstr NUL-terminated source asset path.
static void model_set_root_name(rt_scene_node3d *root, const char *path_cstr) {
    const char *name = path_cstr;
    const char *slash;
    const char *dot;
    size_t len;
    char buf[128];

    if (!root || !path_cstr)
        return;

    slash = strrchr(path_cstr, '/');
    if (!slash)
        slash = strrchr(path_cstr, '\\');
    if (slash && slash[1] != '\0')
        name = slash + 1;

    dot = strrchr(name, '.');
    len = dot && dot > name ? (size_t)(dot - name) : strlen(name);
    if (len == 0)
        return;
    if (len >= sizeof(buf))
        len = sizeof(buf) - 1;
    memcpy(buf, name, len);
    buf[len] = '\0';
    {
        rt_string runtime_name = rt_const_cstr(buf);
        rt_scene_node3d_set_name(root, runtime_name);
        rt_string_unref(runtime_name);
    }
}

/// @brief Recursive deep-copy of a scene-node subtree. Clones TRS, world-dirty flag,
/// visibility, AABB / bounding sphere, and per-LOD entries; *retains* (does not clone)
/// shared assets — `mesh`, `material`, `name`, and per-LOD meshes — so the cloned subtree
/// shares static geometry with the template instead of duplicating GPU resources. Typed node
/// metadata is deep-copied so mutable gameplay state remains instance-local. When requested,
/// morph-enabled meshes are cloned with independent MorphTarget3D state so blend-shape weights
/// can diverge per instance. Children are cloned recursively and parented to the clone via
/// `rt_scene_node3d_try_add_child`, then the caller's local reference is released so the parent
/// owns the only retain.
/// @param mesh Candidate source Mesh3D.
/// @param out_mesh Optional destination for an owned clone when morph state requires isolation.
/// @return One when no clone is needed or cloning succeeds; zero on cloning or validation failure.
static int model_clone_mutable_mesh(void *mesh, void **out_mesh) {
    rt_mesh3d *src = (rt_mesh3d *)rt_g3d_checked_or_null(mesh, RT_G3D_MESH3D_CLASS_ID);
    void *mesh_clone;
    if (out_mesh)
        *out_mesh = NULL;
    if (!src || !src->morph_targets_ref)
        return 1;
    mesh_clone = rt_mesh3d_clone(mesh);
    if (!mesh_clone)
        return 0;
    rt_mesh3d *cloned = (rt_mesh3d *)rt_g3d_checked_or_null(mesh_clone, RT_G3D_MESH3D_CLASS_ID);
    if (!cloned || !cloned->morph_targets_ref) {
        model_release_local(mesh_clone);
        return 0;
    }
    if (out_mesh)
        *out_mesh = mesh_clone;
    else
        model_release_local(mesh_clone);
    return 1;
}

/// @brief Return `value` when finite, else `fallback` (float scalar sanitizer).
/// @param value Candidate float scalar.
/// @param fallback Replacement for non-finite input.
/// @return Finite input or @p fallback.
static float model_finite_float_or(float value, float fallback) {
    return isfinite(value) ? value : fallback;
}

/// @brief Return `value` when finite and ≥ 0, else `fallback`.
/// @param value Candidate double scalar.
/// @param fallback Replacement for negative or non-finite input.
/// @return Non-negative finite input or @p fallback.
static double model_nonnegative_double_or(double value, double fallback) {
    if (!isfinite(value) || value < 0.0)
        return fallback;
    return value;
}

/// @brief Sanitize a cloned node's local AABB: keep the source bounds when finite and
///   well-ordered (min ≤ max on every axis), otherwise fall back to a safe default.
/// @param dst Destination clone receiving sanitized bounds.
/// @param src Source node providing authored bounds.
static void model_clone_sanitize_bounds(rt_scene_node3d *dst, const rt_scene_node3d *src) {
    int valid = 1;
    if (!dst || !src)
        return;
    for (int32_t i = 0; i < 3; i++) {
        if (!isfinite(src->aabb_min[i]) || !isfinite(src->aabb_max[i]) ||
            src->aabb_min[i] > src->aabb_max[i]) {
            valid = 0;
            break;
        }
    }
    if (!valid) {
        memset(dst->aabb_min, 0, sizeof(dst->aabb_min));
        memset(dst->aabb_max, 0, sizeof(dst->aabb_max));
        dst->bsphere_radius = 0.0f;
        return;
    }
    for (int32_t i = 0; i < 3; i++) {
        dst->aabb_min[i] = model_finite_float_or(src->aabb_min[i], 0.0f);
        dst->aabb_max[i] = model_finite_float_or(src->aabb_max[i], dst->aabb_min[i]);
    }
    dst->bsphere_radius =
        (isfinite(src->bsphere_radius) && src->bsphere_radius >= 0.0f) ? src->bsphere_radius : 0.0f;
}

/// @brief Deep-copy one validated native gameplay-metadata table.
/// @param dst Destination node receiving independently owned metadata.
/// @param src Source node with sorted bounded metadata.
/// @return One after complete copy, or zero for invalid metadata or allocation failure.
static int model_clone_node_metadata(rt_scene_node3d *dst, const rt_scene_node3d *src) {
    if (!dst || !src || src->metadata_count < 0 || src->metadata_capacity < 0 ||
        src->metadata_count > src->metadata_capacity ||
        src->metadata_capacity > RT_SCENE_NODE3D_MAX_METADATA_ENTRIES ||
        (src->metadata_count > 0 && !src->metadata))
        return 0;

    for (int32_t index = 0; index < src->metadata_count; ++index) {
        const rt_scene3d_metadata_entry *entry = &src->metadata[index];
        rt_string key;
        int8_t copied = 0;
        if (!entry->key || entry->key_length <= 0 ||
            entry->key_length > RT_SCENE_NODE3D_MAX_METADATA_KEY_BYTES ||
            memchr(entry->key, '\0', (size_t)entry->key_length) != NULL ||
            entry->key[entry->key_length] != '\0' ||
            (index > 0 && strcmp(src->metadata[index - 1].key, entry->key) >= 0))
            return 0;
        key = rt_string_from_bytes(entry->key, (size_t)entry->key_length);
        if (!key)
            return 0;
        switch (entry->kind) {
            case RT_SCENE3D_METADATA_NULL:
                copied = rt_scene_node3d_metadata_set_null(dst, key);
                break;
            case RT_SCENE3D_METADATA_BOOL:
                copied = rt_scene_node3d_metadata_set_bool(dst, key, entry->value.bool_value);
                break;
            case RT_SCENE3D_METADATA_INT:
                copied = rt_scene_node3d_metadata_set_int(dst, key, entry->value.int_value);
                break;
            case RT_SCENE3D_METADATA_FLOAT:
                copied = rt_scene_node3d_metadata_set_float(dst, key, entry->value.float_value);
                break;
            case RT_SCENE3D_METADATA_STRING: {
                rt_string value = NULL;
                if (entry->value.string_value.data && entry->value.string_value.length >= 0 &&
                    entry->value.string_value.length <= RT_SCENE_NODE3D_MAX_METADATA_STRING_BYTES &&
                    memchr(entry->value.string_value.data,
                           '\0',
                           (size_t)entry->value.string_value.length) == NULL &&
                    entry->value.string_value.data[entry->value.string_value.length] == '\0') {
                    value = rt_string_from_bytes(entry->value.string_value.data,
                                                 (size_t)entry->value.string_value.length);
                }
                if (value) {
                    copied = rt_scene_node3d_metadata_set_string(dst, key, value);
                    rt_string_unref(value);
                }
                break;
            }
            default:
                copied = 0;
                break;
        }
        rt_string_unref(key);
        if (!copied)
            return 0;
    }
    return 1;
}

/// @brief Clone one node without its children. Returns NULL and releases partial state on OOM.
/// @details Copies sanitized transforms, bounds, flags, metadata, variants, impostors, and LODs.
///          Static resources are retained; cameras and morph-enabled meshes are cloned when
///          mutable per-instance state is requested.
/// @param src Source SceneNode3D.
/// @param clone_mutable_meshes Nonzero to isolate morph meshes and camera state.
/// @return New owned shallow node clone, or NULL on validation or allocation failure.
static rt_scene_node3d *model_clone_node_shallow(const rt_scene_node3d *src,
                                                 int clone_mutable_meshes) {
    rt_scene_node3d *dst;
    if (!src)
        return NULL;

    dst = (rt_scene_node3d *)rt_scene_node3d_new();
    if (!dst)
        return NULL;

    dst->position[0] = scene3d_clamp_abs_or(src->position[0], 0.0);
    dst->position[1] = scene3d_clamp_abs_or(src->position[1], 0.0);
    dst->position[2] = scene3d_clamp_abs_or(src->position[2], 0.0);
    dst->rotation[0] = src->rotation[0];
    dst->rotation[1] = src->rotation[1];
    dst->rotation[2] = src->rotation[2];
    dst->rotation[3] = src->rotation[3];
    scene3d_quat_normalize_local(dst->rotation);
    dst->scale_xyz[0] = scene3d_scale_or_unit(src->scale_xyz[0]);
    dst->scale_xyz[1] = scene3d_scale_or_unit(src->scale_xyz[1]);
    dst->scale_xyz[2] = scene3d_scale_or_unit(src->scale_xyz[2]);
    dst->world_dirty = 1;
    dst->import_index = src->import_index;
    dst->visible = src->visible ? 1 : 0;
    dst->is_static = src->is_static ? 1 : 0;
    dst->sync_mode = src->sync_mode >= RT_SCENE_NODE3D_SYNC_NODE_FROM_BODY &&
                             src->sync_mode <= RT_SCENE_NODE3D_SYNC_TWO_WAY_KINEMATIC
                         ? src->sync_mode
                         : RT_SCENE_NODE3D_SYNC_NODE_FROM_BODY;
    dst->auto_lod_enabled = src->auto_lod_enabled ? 1 : 0;
    dst->auto_lod_screen_error_px = model_nonnegative_double_or(src->auto_lod_screen_error_px, 8.0);
    if (dst->auto_lod_screen_error_px < 1.0)
        dst->auto_lod_screen_error_px = 1.0;
    if (dst->auto_lod_screen_error_px > 1000000.0)
        dst->auto_lod_screen_error_px = 1000000.0;

    model_clone_sanitize_bounds(dst, src);

    if (src->mesh) {
        void *src_mesh = rt_g3d_checked_or_null(src->mesh, RT_G3D_MESH3D_CLASS_ID);
        void *mesh_clone = NULL;
        if (src_mesh && clone_mutable_meshes && !model_clone_mutable_mesh(src_mesh, &mesh_clone)) {
            model_release_local(dst);
            return NULL;
        }
        if (mesh_clone) {
            dst->mesh = mesh_clone;
        } else if (src_mesh) {
            rt_obj_retain_maybe(src_mesh);
            dst->mesh = src_mesh;
        }
    }
    if (src->material) {
        void *material = rt_g3d_checked_or_null(src->material, RT_G3D_MATERIAL3D_CLASS_ID);
        if (material) {
            rt_obj_retain_maybe(material);
            dst->material = material;
        }
    }
    if (src->light) {
        void *light = rt_g3d_checked_or_null(src->light, RT_G3D_LIGHT3D_CLASS_ID);
        if (light) {
            rt_obj_retain_maybe(light);
            dst->light = light;
        }
    }
    if (src->camera) {
        void *camera = rt_g3d_checked_or_null(src->camera, RT_G3D_CAMERA3D_CLASS_ID);
        if (camera && clone_mutable_meshes) {
            void *camera_clone = rt_camera3d_clone(camera);
            if (!camera_clone) {
                model_release_local(dst);
                return NULL;
            }
            dst->camera = camera_clone;
        } else if (camera) {
            /* Immutable SceneAsset templates share the camera table's exact object so scene
             * membership and node attachment retain one identity. Mutable instances clone it. */
            rt_obj_retain_maybe(camera);
            dst->camera = camera;
        }
    }
    if (src->name && rt_string_is_handle(src->name)) {
        rt_obj_retain_maybe(src->name);
        dst->name = src->name;
    }
    /* VSCN v7 prefab identity (ADR 0187): instances stay instances through
     * SceneAsset instantiation, so Studio and games keep the reference and
     * the transient content flag after every reload path. */
    if (src->prefab_path && rt_string_is_handle(src->prefab_path)) {
        rt_obj_retain_maybe(src->prefab_path);
        dst->prefab_path = src->prefab_path;
    }
    dst->is_instance_content = src->is_instance_content ? 1 : 0;
    if (!model_clone_node_metadata(dst, src)) {
        model_release_local(dst);
        return NULL;
    }

    {
        int32_t lod_count = model_node_lod_count(src);
        if (lod_count <= 0)
            goto after_lods;
        if ((size_t)lod_count > SIZE_MAX / sizeof(*dst->lod_levels)) {
            rt_trap("Model3D: lod allocation overflow");
            model_release_local(dst);
            return NULL;
        }
        dst->lod_levels = calloc((size_t)lod_count, sizeof(*dst->lod_levels));
        if (!dst->lod_levels) {
            rt_trap("Model3D: lod allocation failed");
            model_release_local(dst);
            return NULL;
        }
        dst->lod_capacity = lod_count;
        for (int32_t i = 0; i < lod_count; i++) {
            void *lod_mesh_clone = NULL;
            void *lod_mesh =
                rt_g3d_checked_or_null(src->lod_levels[i].mesh, RT_G3D_MESH3D_CLASS_ID);
            double distance = model_nonnegative_double_or(src->lod_levels[i].distance, 0.0);
            if (dst->lod_count > 0 && distance < dst->lod_levels[dst->lod_count - 1].distance)
                distance = dst->lod_levels[dst->lod_count - 1].distance;
            if (!lod_mesh)
                continue;
            if (clone_mutable_meshes && !model_clone_mutable_mesh(lod_mesh, &lod_mesh_clone)) {
                model_release_local(dst);
                return NULL;
            }
            dst->lod_levels[dst->lod_count].distance = distance;
            if (lod_mesh_clone) {
                dst->lod_levels[dst->lod_count].mesh = lod_mesh_clone;
            } else {
                dst->lod_levels[dst->lod_count].mesh = lod_mesh;
                rt_obj_retain_maybe(dst->lod_levels[dst->lod_count].mesh);
            }
            dst->lod_count++;
        }
    }
after_lods:

    if (src->has_impostor && src->impostor_pixels) {
        double impostor_distance = model_nonnegative_double_or(src->impostor_distance, 0.0);
        rt_scene_node3d_set_impostor(dst, impostor_distance, src->impostor_pixels);
    }

    if (!rt_scene_node3d_copy_variant_materials(dst, src)) {
        model_release_local(dst);
        return NULL;
    }

    return dst;
}

typedef struct model_clone_frame {
    const rt_scene_node3d *src;
    rt_scene_node3d *dst;
    int32_t next_child;
} model_clone_frame;

/// @brief Iterative deep-copy of a scene-node subtree.
/// @param src Source subtree root.
/// @param clone_mutable_meshes Nonzero to isolate mutable mesh and camera state.
/// @return New owned subtree root, or NULL on validation, limit, or allocation failure.
static rt_scene_node3d *model_clone_node(const rt_scene_node3d *src, int clone_mutable_meshes) {
    rt_scene_node3d *root;
    model_clone_frame *stack;
    int32_t stack_count = 0;
    int32_t stack_capacity = 32;
    int32_t visited_count = 1;
    if (!src)
        return NULL;
    root = model_clone_node_shallow(src, clone_mutable_meshes);
    if (!root)
        return NULL;
    stack = (model_clone_frame *)malloc((size_t)stack_capacity * sizeof(*stack));
    if (!stack) {
        rt_trap("Model3D: clone stack allocation failed");
        model_release_local(root);
        return NULL;
    }
    stack[stack_count++] = (model_clone_frame){src, root, 0};
    while (stack_count > 0) {
        model_clone_frame *frame = &stack[stack_count - 1];
        int32_t child_count = model_node_child_count(frame->src);
        if (!frame->src || frame->next_child >= child_count) {
            stack_count--;
            continue;
        }
        const rt_scene_node3d *src_child = (const rt_scene_node3d *)rt_g3d_checked_or_null(
            frame->src->children[frame->next_child++], RT_G3D_SCENENODE3D_CLASS_ID);
        if (!src_child)
            continue;
        if (visited_count >= MODEL3D_MAX_WALK_NODES) {
            free(stack);
            model_release_local(root);
            rt_trap("Model3D: too many nodes to clone");
            return NULL;
        }
        visited_count++;
        rt_scene_node3d *dst_child = model_clone_node_shallow(src_child, clone_mutable_meshes);
        if (!dst_child) {
            free(stack);
            model_release_local(root);
            return NULL;
        }
        if (!rt_scene_node3d_try_add_child(frame->dst, dst_child)) {
            model_release_local(dst_child);
            free(stack);
            model_release_local(root);
            return NULL;
        }
        model_release_local(dst_child);
        if (stack_count >= stack_capacity) {
            if (stack_capacity > INT32_MAX / 2 ||
                (size_t)(stack_capacity * 2) > SIZE_MAX / sizeof(*stack)) {
                free(stack);
                model_release_local(root);
                rt_trap("Model3D: too many nodes to clone");
                return NULL;
            }
            int32_t new_capacity = stack_capacity * 2;
            model_clone_frame *grown =
                (model_clone_frame *)realloc(stack, (size_t)new_capacity * sizeof(*stack));
            if (!grown) {
                free(stack);
                model_release_local(root);
                rt_trap("Model3D: clone stack allocation failed");
                return NULL;
            }
            stack = grown;
            stack_capacity = new_capacity;
        }
        stack[stack_count++] = (model_clone_frame){src_child, dst_child, 0};
    }
    free(stack);
    return root;
}

/// @brief Clone each child of `src_root` and attach to `dst_root`. Used by `Model3D.Load`
/// to graft the imported scene's top-level children under the synthesized template root
/// *without* copying the import's own root (which often carries loader-specific metadata
/// we don't want to leak into the user-visible tree).
/// @param dst_root Synthetic destination root receiving copied metadata and cloned children.
/// @param src_root Imported source root.
/// @return One after all children are attached, or zero on invalid input or cloning failure.
static int model_clone_children_to_root(rt_scene_node3d *dst_root,
                                        const rt_scene_node3d *src_root) {
    if (!dst_root || !src_root)
        return 0;
    /* Scene-scoped root metadata (bake.*, env.* conventions) travels with
     * the template so instantiation matches direct SceneGraph.Load. */
    if (!model_clone_node_metadata(dst_root, src_root))
        return 0;
    for (int32_t i = 0, child_count = model_node_child_count(src_root); i < child_count; i++) {
        const rt_scene_node3d *src_child = (const rt_scene_node3d *)rt_g3d_checked_or_null(
            src_root->children[i], RT_G3D_SCENENODE3D_CLASS_ID);
        if (!src_child)
            continue;
        rt_scene_node3d *child = model_clone_node(src_child, 0);
        if (!child)
            return 0;
        if (!rt_scene_node3d_try_add_child(dst_root, child)) {
            model_release_local(child);
            return 0;
        }
        model_release_local(child);
    }
    return 1;
}

static int model_collect_scene_refs(rt_model3d *model, const rt_scene_node3d *node);

/// @brief Collect mesh/material references from every top-level child of the template root.
/// @details Delegates to `model_collect_scene_refs` for each immediate child so the template
///   root node itself is not registered (it is a synthetic container, not a content node).
///   Returns 1 on success, 0 if any registration step fails due to an allocation error.
/// @param model Model3D whose retained inventories are populated.
/// @return One after collection or for an empty template, or zero on allocation failure.
static int model_collect_template_refs(rt_model3d *model) {
    if (!model || !model->template_root)
        return 1;
    for (int32_t i = 0, child_count = model_node_child_count(model->template_root); i < child_count;
         i++) {
        const rt_scene_node3d *child = (const rt_scene_node3d *)rt_g3d_checked_or_null(
            model->template_root->children[i], RT_G3D_SCENENODE3D_CLASS_ID);
        if (child && !model_collect_scene_refs(model, child))
            return 0;
    }
    return 1;
}

/// @brief Wire a default skeletal AnimationController3D onto `root` when one is present in the
/// model.
/// @details Creates an AnimController3D backed by the first skeleton, adds one state per loaded
///   animation clip (naming it after its rt_animation3d name, with a "animation_<i>" fallback for
///   unnamed clips), then plays the first state so the model arrives in a live pose immediately
///   after instantiation.  No-op when the root already has a bound animator, there are no
///   skeletons, or there are no animation clips to add.  The controller's local reference is
///   released after binding because the root's `bound_animator` slot owns the retain. When an
///   importer produces more than one skeleton, the first valid skeleton is used for the default
///   controller so multi-skin assets still get a controllable default instead of no playback.
/// @param model Model3D providing skeletons and animation clips.
/// @param root Instantiated root receiving the controller when eligible.
static void model_bind_default_animator(rt_model3d *model, rt_scene_node3d *root) {
    void *controller;
    int added_any = 0;
    void *skeleton = NULL;
    int32_t valid_skeleton_count = 0;
    int32_t skeleton_count = model ? model_clamped_array_count(model->skeletons,
                                                               model->skeleton_count,
                                                               model->skeleton_capacity)
                                   : 0;
    int32_t animation_count = model ? model_clamped_array_count(model->animations,
                                                                model->animation_count,
                                                                model->animation_capacity)
                                    : 0;
    if (!model || !root || root->bound_animator || skeleton_count <= 0 || animation_count <= 0)
        return;
    for (int32_t i = 0; i < skeleton_count; i++) {
        void *candidate = rt_g3d_checked_or_null(model->skeletons[i], RT_G3D_SKELETON3D_CLASS_ID);
        if (!candidate)
            continue;
        if (!skeleton)
            skeleton = candidate;
        valid_skeleton_count++;
    }
    if (valid_skeleton_count <= 0 || !skeleton)
        return;
    controller = rt_anim_controller3d_new(skeleton);
    if (!controller)
        return;
    for (int32_t i = 0; i < animation_count; i++) {
        rt_string anim_name;
        const char *name;
        char fallback[64];
        char unique_fallback[64];
        rt_string state_name;
        int64_t state_index;
        int64_t before_count;
        void *animation = rt_g3d_checked_or_null(model->animations[i], RT_G3D_ANIMATION3D_CLASS_ID);
        if (!animation)
            continue;
        anim_name = rt_animation3d_get_name(animation);
        name = anim_name ? rt_string_cstr(anim_name) : NULL;
        if (!name || name[0] == '\0') {
            snprintf(fallback, sizeof(fallback), "animation_%d", (int)i);
            name = fallback;
        }
        state_name = rt_const_cstr(name);
        before_count = rt_anim_controller3d_get_state_count(controller);
        state_index = rt_anim_controller3d_add_state(controller, state_name, animation);
        if (state_index >= 0 && rt_anim_controller3d_get_state_count(controller) == before_count &&
            state_index < before_count) {
            snprintf(unique_fallback, sizeof(unique_fallback), "animation_%d", (int)i);
            if (strcmp(unique_fallback, name) != 0) {
                rt_string_unref(state_name);
                state_name = rt_const_cstr(unique_fallback);
                state_index = rt_anim_controller3d_add_state(controller, state_name, animation);
            }
        }
        if (state_index >= 0 && !added_any) {
            rt_anim_controller3d_play(controller, state_name);
            added_any = 1;
        }
        rt_string_unref(state_name);
        rt_string_unref(anim_name);
    }
    if (added_any)
        rt_scene_node3d_bind_animator(root, controller);
    model_release_local(controller);
}

/// @brief Wire a default NodeAnimator3D onto `root` using any node-animation clips in the model.
/// @details Creates a NodeAnimator3D from the full `node_animations` array so scene-node TRS
///   animations (camera paths, object tracks) play automatically after instantiation, mirroring
///   the same auto-play convenience as `model_bind_default_animator` for skeletal animations.
///   No-op when a node animator is already bound or when no node-animation clips were loaded.
///   The animator's local reference is released after binding.
/// @param model Model3D providing node-animation clips.
/// @param root Instantiated root receiving the animator when eligible.
static void model_bind_default_node_animator(rt_model3d *model, rt_scene_node3d *root) {
    void *animator;
    void **clips = NULL;
    int32_t live_count = 0;
    int32_t node_animation_count = model ? model_clamped_array_count(model->node_animations,
                                                                     model->node_animation_count,
                                                                     model->node_animation_capacity)
                                         : 0;
    if (!model || !root || root->bound_node_animator || node_animation_count <= 0)
        return;
    for (int32_t i = 0; i < node_animation_count; i++) {
        if (rt_g3d_checked_or_null(model->node_animations[i], RT_G3D_NODEANIMATION3D_CLASS_ID))
            live_count++;
    }
    if (live_count <= 0)
        return;
    if (live_count == node_animation_count) {
        animator = rt_node_animator3d_new_from_clips(model->node_animations, node_animation_count);
    } else {
        if ((size_t)live_count > SIZE_MAX / sizeof(void *)) {
            rt_trap("Model3D.Instantiate: node-animation list too large");
            return;
        }
        clips = (void **)malloc((size_t)live_count * sizeof(void *));
        if (!clips) {
            rt_trap("Model3D.Instantiate: node-animation list allocation failed");
            return;
        }
        for (int32_t i = 0, out_i = 0; i < node_animation_count; i++) {
            void *clip =
                rt_g3d_checked_or_null(model->node_animations[i], RT_G3D_NODEANIMATION3D_CLASS_ID);
            if (clip)
                clips[out_i++] = clip;
        }
        animator = rt_node_animator3d_new_from_clips(clips, live_count);
        free(clips);
    }
    if (!animator)
        return;
    rt_scene_node3d_bind_node_animator(root, animator);
    model_release_local(animator);
}

/// @brief Walk the scene subtree at `node` and register every unique mesh, attached skeleton,
/// and material reference (including meshes stored on LOD entries) into the model arrays.
/// Used after cloning imported scene-graph nodes so consumers can enumerate the assets
/// without re-walking the tree. Recurses into every child; duplicates are skipped by
/// `model_append_unique_ref`.
/// @param model Destination Model3D inventory.
/// @param node Source subtree root.
/// @return One after collecting all valid unique references, or zero on limit/allocation failure.
static int model_collect_scene_refs(rt_model3d *model, const rt_scene_node3d *node) {
    const rt_scene_node3d **stack = NULL;
    int32_t stack_count = 0;
    int32_t stack_capacity = 32;
    int32_t visited_count = 1;
    static const char *too_many_msg = "Model3D.Load: too many scene nodes";
    if (!model || !node)
        return 1;
    stack = (const rt_scene_node3d **)malloc((size_t)stack_capacity * sizeof(*stack));
    if (!stack) {
        rt_trap("Model3D.Load: scene-ref walk allocation failed");
        return 0;
    }
    stack[stack_count++] = node;
    while (stack_count > 0) {
        const rt_scene_node3d *current = stack[--stack_count];
        void *material;
        if (!current)
            continue;
        material = rt_g3d_checked_or_null(current->material, RT_G3D_MATERIAL3D_CLASS_ID);
        if (!model_collect_mesh_refs(model, current->mesh)) {
            free(stack);
            return 0;
        }
        if (!model_append_unique_ref(&model->materials,
                                     &model->material_count,
                                     &model->material_capacity,
                                     material,
                                     "Model3D.Load: material list allocation failed")) {
            free(stack);
            return 0;
        }
        int32_t lod_count = model_node_lod_count(current);
        int32_t child_count = model_node_child_count(current);
        for (int32_t i = 0; i < lod_count; i++) {
            if (!model_collect_mesh_refs(model, current->lod_levels[i].mesh)) {
                free(stack);
                return 0;
            }
        }
        for (int32_t i = 0; i < current->variant_material_count && current->variant_materials;
             i++) {
            void *variant_material =
                rt_g3d_checked_or_null(current->variant_materials[i], RT_G3D_MATERIAL3D_CLASS_ID);
            if (!model_append_unique_ref(&model->materials,
                                         &model->material_count,
                                         &model->material_capacity,
                                         variant_material,
                                         "Model3D.Load: material list allocation failed")) {
                free(stack);
                return 0;
            }
        }
        for (int32_t i = 0; i < child_count; i++) {
            const rt_scene_node3d *child = (const rt_scene_node3d *)rt_g3d_checked_or_null(
                current->children[i], RT_G3D_SCENENODE3D_CLASS_ID);
            if (!child)
                continue;
            if (visited_count >= MODEL3D_MAX_WALK_NODES) {
                free(stack);
                rt_trap(too_many_msg);
                return 0;
            }
            if (stack_count >= stack_capacity) {
                if (stack_capacity > INT32_MAX / 2 ||
                    (size_t)(stack_capacity * 2) > SIZE_MAX / sizeof(rt_scene_node3d *)) {
                    free(stack);
                    rt_trap(too_many_msg);
                    return 0;
                }
                int32_t new_capacity = stack_capacity * 2;
                const rt_scene_node3d **grown = (const rt_scene_node3d **)realloc(
                    stack, (size_t)new_capacity * sizeof(rt_scene_node3d *));
                if (!grown) {
                    free(stack);
                    rt_trap("Model3D.Load: scene-ref walk allocation failed");
                    return 0;
                }
                stack = grown;
                stack_capacity = new_capacity;
            }
            stack[stack_count++] = child;
            visited_count++;
        }
    }
    free(stack);
    return 1;
}

/// @brief Find a named node using Model3D's clamped child-count walker.
/// @param root Subtree root to search.
/// @param name Valid runtime string containing the exact name.
/// @return Borrowed matching node, or NULL when absent, invalid, or traversal fails.
static rt_scene_node3d *model_find_node_safe(rt_scene_node3d *root, rt_string name) {
    rt_scene_node3d **stack = NULL;
    int32_t stack_count = 0;
    int32_t stack_capacity = 32;
    int32_t visited_count = 1;
    const char *needle = model_string_cstr_or_null(name);
    if (!root || !needle)
        return NULL;
    stack = (rt_scene_node3d **)malloc((size_t)stack_capacity * sizeof(*stack));
    if (!stack) {
        rt_trap("Model3D.FindNode: search stack allocation failed");
        return NULL;
    }
    stack[stack_count++] = root;
    while (stack_count > 0) {
        rt_scene_node3d *current = stack[--stack_count];
        int32_t child_count;
        if (!current)
            continue;
        if (current->name) {
            const char *node_name = model_string_cstr_or_null(current->name);
            if (node_name && strcmp(node_name, needle) == 0) {
                free(stack);
                return current;
            }
        }
        child_count = model_node_child_count(current);
        for (int32_t i = 0; i < child_count; i++) {
            rt_scene_node3d *child = (rt_scene_node3d *)rt_g3d_checked_or_null(
                current->children[i], RT_G3D_SCENENODE3D_CLASS_ID);
            if (!child)
                continue;
            if (visited_count >= MODEL3D_MAX_WALK_NODES) {
                free(stack);
                rt_trap("Model3D.FindNode: too many nodes");
                return NULL;
            }
            if (stack_count >= stack_capacity) {
                if (stack_capacity > INT32_MAX / 2 ||
                    (size_t)(stack_capacity * 2) > SIZE_MAX / sizeof(rt_scene_node3d *)) {
                    free(stack);
                    rt_trap("Model3D.FindNode: too many nodes");
                    return NULL;
                }
                int32_t new_capacity = stack_capacity * 2;
                rt_scene_node3d **grown = (rt_scene_node3d **)realloc(
                    stack, (size_t)new_capacity * sizeof(rt_scene_node3d *));
                if (!grown) {
                    free(stack);
                    rt_trap("Model3D.FindNode: search stack allocation failed");
                    return NULL;
                }
                stack = grown;
                stack_capacity = new_capacity;
            }
            stack[stack_count++] = child;
            visited_count++;
        }
    }
    free(stack);
    return NULL;
}

/// @brief Case-insensitive extension match. Returns 1 when `path_cstr` ends with `ext`
/// (which must start with `.`), ignoring ASCII case. Used by `Model3D.Load` to pick the
/// right importer (.scene3d/.vscn / .gltf / .glb / .fbx) without relying on
/// platform-dependent filename normalization.
/// @param path_cstr NUL-terminated candidate path.
/// @param ext NUL-terminated extension beginning with a period.
/// @return One for an ASCII case-insensitive suffix match; otherwise zero.
static int model_has_ext(const char *path_cstr, const char *ext) {
    const char *dot;
    if (!path_cstr || !ext)
        return 0;
    dot = strrchr(path_cstr, '.');
    if (!dot)
        return 0;
    while (*dot && *ext) {
        if (tolower((unsigned char)*dot) != tolower((unsigned char)*ext))
            return 0;
        ++dot;
        ++ext;
    }
    return *dot == '\0' && *ext == '\0';
}

/// @brief Synthesize a minimal scene-graph when the imported asset only exposes a bare
/// mesh/material list (no native scene hierarchy). One `mesh_<i>` child per mesh gets
/// parented under the template root, with material assignment: single shared material
/// if the asset has exactly one, otherwise parallel indexing by mesh slot. No-op when
/// the template root already has children (i.e. the importer gave us a real hierarchy).
/// @param model Model3D whose empty template may receive synthetic mesh nodes.
/// @return One when a hierarchy already exists or synthesis succeeds; zero on invalid input or
///         allocation/parenting failure.
static int model_build_synth_mesh_nodes(rt_model3d *model) {
    char name[64];
    if (!model || !model->template_root)
        return 0;
    if (model_node_child_count(model->template_root) > 0)
        return 1;

    int32_t mesh_count =
        model_clamped_array_count(model->meshes, model->mesh_count, model->mesh_capacity);
    int32_t material_count = model_clamped_array_count(
        model->materials, model->material_count, model->material_capacity);
    for (int32_t i = 0; i < mesh_count; i++) {
        rt_scene_node3d *node = (rt_scene_node3d *)rt_scene_node3d_new();
        void *material = NULL;
        if (!model->meshes[i])
            continue;
        if (!node)
            return 0;
        snprintf(name, sizeof(name), "mesh_%d", (int)i);
        {
            rt_string runtime_name = rt_const_cstr(name);
            rt_scene_node3d_set_name(node, runtime_name);
            rt_string_unref(runtime_name);
        }
        rt_scene_node3d_set_mesh(node, model->meshes[i]);
        if (material_count == 1)
            material = model->materials[0];
        else if (i < material_count)
            material = model->materials[i];
        if (material)
            rt_scene_node3d_set_material(node, material);
        if (!rt_scene_node3d_try_add_child(model->template_root, node)) {
            model_release_local(node);
            return 0;
        }
        model_release_local(node);
    }
    return 1;
}

typedef struct {
    char *name;
    void *material;
} model_obj_material_entry;

typedef struct {
    model_obj_material_entry *entries;
    int32_t count;
    int32_t capacity;
} model_obj_material_table;

/// @brief Safe live entry count for the OBJ material table.
/// @param table Material lookup table.
/// @return Safe count bounded by valid storage and capacity.
static int32_t model_obj_material_table_count(const model_obj_material_table *table) {
    return table ? model_clamped_array_count(table->entries, table->count, table->capacity) : 0;
}

/// @brief Clamp and persist the OBJ material table count before mutation.
/// @param table Material table whose count is repaired.
/// @return Repaired safe entry count.
static int32_t model_obj_material_table_repair_count(model_obj_material_table *table) {
    int32_t count = model_obj_material_table_count(table);
    if (table)
        table->count = count;
    return count;
}

/// @brief Duplicate the substring [@p start, @p end), trimming leading/trailing ASCII
///   whitespace. Returns NULL on allocation failure.
/// @param start First source byte, or NULL for an empty string.
/// @param end One-past-end source pointer, or NULL to use the source terminator.
/// @return Newly allocated trimmed string, or NULL on allocation failure.
static char *model_strdup_range(const char *start, const char *end) {
    size_t len;
    char *copy;
    if (!start)
        start = "";
    if (!end)
        end = start + strlen(start);
    while (start < end && (*start == ' ' || *start == '\t'))
        start++;
    while (end > start && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\n' || end[-1] == '\r'))
        end--;
    len = (size_t)(end - start);
    copy = (char *)malloc(len + 1u);
    if (!copy)
        return NULL;
    memcpy(copy, start, len);
    copy[len] = '\0';
    return copy;
}

/// @brief Free an OBJ material table: release each entry's name and material reference, then
///   the entry array, and zero the table.
/// @param table Owned table to release; NULL is ignored.
static void model_obj_material_table_free(model_obj_material_table *table) {
    if (!table)
        return;
    if (!table->entries) {
        table->count = 0;
        table->capacity = 0;
        return;
    }
    for (int32_t i = 0, count = model_obj_material_table_count(table); i < count; i++) {
        free(table->entries[i].name);
        model_release_ref(&table->entries[i].material);
    }
    free(table->entries);
    memset(table, 0, sizeof(*table));
}

/// @brief Append a (name, material) entry, de-duplicating by name (on a duplicate, frees the
///   passed @p name and releases @p material). Takes ownership of @p name and the material
///   reference and grows the table as needed.
/// @param table Destination material table.
/// @param name Owned material-name allocation consumed by this call.
/// @param material Owned Material3D reference consumed by this call.
/// @return 1 on success or a harmless duplicate; 0 on allocation failure.
static int model_obj_material_table_append(model_obj_material_table *table,
                                           char *name,
                                           void *material) {
    model_obj_material_entry *grown;
    int32_t count;
    int32_t old_capacity;
    if (!table || !name || !material)
        return 0;
    count = model_obj_material_table_repair_count(table);
    for (int32_t i = 0; i < count; i++) {
        if (strcmp(table->entries[i].name ? table->entries[i].name : "", name) == 0) {
            free(name);
            model_release_local(material);
            return 1;
        }
    }
    if (!table->entries)
        table->capacity = 0;
    if (table->capacity < 0)
        table->capacity = 0;
    old_capacity = table->capacity;
    if (count >= table->capacity) {
        int32_t new_capacity = old_capacity > 0 ? old_capacity * 2 : 8;
        if (old_capacity > INT32_MAX / 2 ||
            (size_t)new_capacity > SIZE_MAX / sizeof(*table->entries))
            return 0;
        grown = (model_obj_material_entry *)realloc(table->entries,
                                                    (size_t)new_capacity * sizeof(*grown));
        if (!grown)
            return 0;
        memset(grown + old_capacity, 0, (size_t)(new_capacity - old_capacity) * sizeof(*grown));
        table->entries = grown;
        table->capacity = new_capacity;
    }
    table->entries[count].name = name;
    table->entries[count].material = material;
    table->count = count + 1;
    return 1;
}

/// @brief Find a material by name in the OBJ material table; NULL if absent.
/// @param table Material lookup table.
/// @param name Exact nonempty material name.
/// @return Borrowed Material3D reference, or NULL when absent or invalid.
static void *model_obj_material_lookup(const model_obj_material_table *table, const char *name) {
    if (!table || !name || !*name)
        return NULL;
    for (int32_t i = 0, count = model_obj_material_table_count(table); i < count; i++) {
        if (strcmp(table->entries[i].name ? table->entries[i].name : "", name) == 0)
            return table->entries[i].material;
    }
    return NULL;
}

/// @brief Write the parent-directory portion of @p path (up to the last '/' or '\\') into
///   @p out; empty if @p path has no separator.
/// @param out Destination character buffer.
/// @param out_size Writable size of @p out.
/// @param path Nullable source path.
static void model_parent_dir(char *out, size_t out_size, const char *path) {
    const char *last;
    size_t len;
    if (!out || out_size == 0)
        return;
    out[0] = '\0';
    if (!path)
        return;
    {
        const char *slash = strrchr(path, '/');
        const char *backslash = strrchr(path, '\\');
        last = (!slash || (backslash && backslash > slash)) ? backslash : slash;
    }
    if (!last)
        return;
    len = (size_t)(last - path);
    if (len == 0 && (path[0] == '/' || path[0] == '\\'))
        len = 1;
    if (len >= out_size)
        len = out_size - 1u;
    memcpy(out, path, len);
    out[len] = '\0';
}

/// @brief True if @p path is absolute: a leading '/' or '\\', or a Windows drive letter ("C:").
/// @param path Candidate NUL-terminated path.
/// @return Nonzero for Unix-rooted, backslash-rooted, or drive-qualified paths.
static int model_is_absolute_path(const char *path) {
    if (!path || !*path)
        return 0;
    if (path[0] == '/' || path[0] == '\\')
        return 1;
    return isalpha((unsigned char)path[0]) && path[1] == ':';
}

/// @brief Sanitize an asset reference into a safe relative path in @p out (path-traversal
///   guard): converts '\\' to '/', then rejects absolute paths, URI scheme separators, and any
///   ".." segment, collapsing "." and redundant slashes.
/// @param src Untrusted asset reference.
/// @param out Destination for the normalized relative path.
/// @param out_size Writable size of @p out.
/// @return Non-zero if a non-empty safe path was written; 0 if empty, unsafe, or overflowing.
static int model_normalize_relative_asset_ref(const char *src, char *out, size_t out_size) {
    char normalized[MODEL3D_OBJ_MAX_PATH_BYTES];
    const char *p;
    size_t ni = 0;
    size_t out_len = 0;
    int wrote_segment = 0;
    if (!src || !*src || !out || out_size == 0)
        return 0;
    while (*src && ni + 1u < sizeof(normalized)) {
        char ch = *src++;
        unsigned char uch = (unsigned char)ch;
        if (ch == ':')
            return 0;
        if (uch < 0x20u || uch == 0x7fu)
            return 0;
        normalized[ni++] = ch == '\\' ? '/' : ch;
    }
    if (*src)
        return 0;
    normalized[ni] = '\0';
    if (model_is_absolute_path(normalized) || strstr(normalized, "://"))
        return 0;
    p = normalized;
    out[0] = '\0';
    while (*p) {
        const char *seg;
        size_t seg_len;
        while (*p == '/')
            p++;
        seg = p;
        while (*p && *p != '/')
            p++;
        seg_len = (size_t)(p - seg);
        if (seg_len == 0 || (seg_len == 1 && seg[0] == '.'))
            continue;
        if (seg_len == 2 && seg[0] == '.' && seg[1] == '.')
            return 0;
        if (wrote_segment) {
            if (out_len + 1u >= out_size)
                return 0;
            out[out_len++] = '/';
        }
        if (seg_len > out_size - 1u - out_len)
            return 0;
        memcpy(out + out_len, seg, seg_len);
        out_len += seg_len;
        out[out_len] = '\0';
        wrote_segment = 1;
    }
    return wrote_segment;
}

/// @brief Join @p dir and @p leaf with a '/' separator into @p out (just @p leaf when @p dir
///   is empty), bounds-checked.
/// @param out Destination character buffer.
/// @param out_size Writable size of @p out.
/// @param dir Nullable parent directory.
/// @param leaf Nonempty relative leaf path.
/// @return One on success, or zero on bad input or overflow.
static int MODEL3D_UNUSED_PRIVATE model_join_path(char *out,
                                                  size_t out_size,
                                                  const char *dir,
                                                  const char *leaf) {
    size_t dir_len;
    size_t leaf_len;
    if (!out || out_size == 0 || !leaf || !*leaf)
        return 0;
    leaf_len = strlen(leaf);
    if (!dir || !*dir) {
        if (leaf_len >= out_size)
            return 0;
        memcpy(out, leaf, leaf_len);
        out[leaf_len] = '\0';
        return 1;
    }
    dir_len = strlen(dir);
    if (dir_len > 0 && (dir[dir_len - 1u] == '/' || dir[dir_len - 1u] == '\\')) {
        if (dir_len + leaf_len >= out_size)
            return 0;
        memcpy(out, dir, dir_len);
        memcpy(out + dir_len, leaf, leaf_len);
        out[dir_len + leaf_len] = '\0';
        return 1;
    }
    if (dir_len + 1u + leaf_len >= out_size)
        return 0;
    memcpy(out, dir, dir_len);
    out[dir_len++] = '/';
    memcpy(out + dir_len, leaf, leaf_len);
    out[dir_len + leaf_len] = '\0';
    return 1;
}

/// @brief Allocate a joined path from @p dir and @p leaf, preserving `model_join_path` semantics.
/// @details This is used by MTL parsing where valid project-relative texture names can
///          exceed the legacy 1 KiB stack buffers. The result is capped at
///          MODEL3D_OBJ_MAX_PATH_BYTES so malformed assets cannot request unbounded
///          memory through a path directive.
/// @param dir Nullable parent directory.
/// @param leaf Nonempty relative leaf path.
/// @return A malloc-owned joined path, or NULL on invalid input, overflow, or allocation failure.
static char *model_join_path_alloc(const char *dir, const char *leaf) {
    size_t dir_len;
    size_t leaf_len;
    size_t total_len;
    int needs_sep;
    char *out;
    if (!leaf || !*leaf)
        return NULL;
    leaf_len = strlen(leaf);
    dir_len = (dir && *dir) ? strlen(dir) : 0;
    needs_sep = dir_len > 0 && dir[dir_len - 1u] != '/' && dir[dir_len - 1u] != '\\';
    if (dir_len > MODEL3D_OBJ_MAX_PATH_BYTES || leaf_len > MODEL3D_OBJ_MAX_PATH_BYTES)
        return NULL;
    if (dir_len > SIZE_MAX - leaf_len - (needs_sep ? 2u : 1u))
        return NULL;
    total_len = dir_len + leaf_len + (needs_sep ? 1u : 0u);
    if (total_len == 0 || total_len >= MODEL3D_OBJ_MAX_PATH_BYTES)
        return NULL;
    out = (char *)malloc(total_len + 1u);
    if (!out)
        return NULL;
    if (dir_len > 0)
        memcpy(out, dir, dir_len);
    if (needs_sep)
        out[dir_len++] = '/';
    memcpy(out + dir_len, leaf, leaf_len);
    out[dir_len + leaf_len] = '\0';
    return out;
}

/// @brief Read one text line into a dynamically resized buffer with an explicit byte cap.
/// @details The buffer grows geometrically and is NUL-terminated. Returns 1 for a
///          line, 0 for EOF before any bytes, -1 for allocation/input failure, and
///          -2 when the line exceeds @p max_bytes.
/// @param f Open text stream.
/// @param line Address of the reusable owned line buffer.
/// @param cap Address of the buffer capacity.
/// @param max_bytes Maximum line-buffer size including termination space.
/// @return One for a line, zero for clean EOF, negative one for invalid/allocation failure, or
///         negative two for an oversized line.
static int model_read_text_line(FILE *f, char **line, size_t *cap, size_t max_bytes) {
    size_t len = 0;
    int ch;
    if (!f || !line || !cap || max_bytes < 2)
        return -1;
    if (!*line || *cap == 0) {
        *cap = max_bytes < 256u ? max_bytes : 256u;
        *line = (char *)malloc(*cap);
        if (!*line) {
            *cap = 0;
            return -1;
        }
    }
    while ((ch = fgetc(f)) != EOF) {
        if (len + 1u >= *cap) {
            size_t new_cap;
            char *grown;
            if (*cap > SIZE_MAX / 2u)
                return -1;
            new_cap = *cap * 2u;
            if (new_cap > max_bytes)
                new_cap = max_bytes;
            if (new_cap <= *cap)
                return -2;
            grown = (char *)realloc(*line, new_cap);
            if (!grown)
                return -1;
            *line = grown;
            *cap = new_cap;
        }
        (*line)[len++] = (char)ch;
        if (len >= max_bytes)
            return -2;
        if (ch == '\n')
            break;
    }
    if (ch == EOF && len == 0)
        return 0;
    (*line)[len] = '\0';
    return 1;
}

/// @brief Advance past leading spaces and tabs in @p p (NULL-safe).
/// @param p Nullable text cursor.
/// @return The first non-blank character, or @p p unchanged when it is NULL.
static const char *model_obj_skip_ws(const char *p) {
    while (p && (*p == ' ' || *p == '\t'))
        p++;
    return p;
}

/// @brief Test whether the remainder of @p p (after leading blanks) carries no further tokens —
///   only trailing whitespace, a newline, or an OBJ/MTL "#" comment follows.
/// @param p Nullable remainder cursor.
/// @return Non-zero when the rest of the line is empty.
static int model_obj_line_tail_done(const char *p) {
    p = model_obj_skip_ws(p);
    return !p || *p == '\0' || *p == '\n' || *p == '\r' || *p == '#';
}

/// @brief Parse one whitespace-delimited numeric token from *@p cursor into @p out, advancing
///   @p cursor past it. Rejects tokens that fill the 128-byte scratch buffer or parse to a
///   non-finite value.
/// @param cursor Address of the source cursor, advanced past the token.
/// @param out Receives the finite parsed value.
/// @return Non-zero on success; 0 at end-of-line, on a "#" comment, or on a malformed/oversized
/// token.
static int model_obj_parse_double_token(const char **cursor, double *out) {
    char token[128];
    const char *start;
    size_t len;
    if (!cursor || !*cursor || !out)
        return 0;
    start = model_obj_skip_ws(*cursor);
    if (!start || *start == '\0' || *start == '\n' || *start == '\r' || *start == '#')
        return 0;
    *cursor = start;
    while (**cursor && **cursor != ' ' && **cursor != '\t' && **cursor != '\n' &&
           **cursor != '\r' && **cursor != '#')
        (*cursor)++;
    len = (size_t)(*cursor - start);
    if (len == 0 || len >= sizeof(token))
        return 0;
    memcpy(token, start, len);
    token[len] = '\0';
    return rt_parse_double(token, out) == 0 && isfinite(*out);
}

/// @brief Parse a line carrying exactly one double (e.g. an MTL scalar such as "Ns"/"d"): one
///   token followed only by blanks or a comment.
/// @param p Source line remainder.
/// @param a Receives the parsed value.
/// @return Non-zero when @p a was read and nothing but trailing whitespace remains.
static int model_obj_parse_double1(const char *p, double *a) {
    const char *cursor = p;
    return model_obj_parse_double_token(&cursor, a) && model_obj_line_tail_done(cursor);
}

/// @brief Parse a line carrying exactly three doubles (e.g. an OBJ "v"/"vn" position/normal or an
///   MTL "Kd"/"Ka"/"Ks" color triple).
/// @param p Source line remainder.
/// @param a Receives the first value.
/// @param b Receives the second value.
/// @param c Receives the third value.
/// @return Non-zero when @p a, @p b and @p c were all read and nothing but trailing whitespace
/// remains.
static int model_obj_parse_double3(const char *p, double *a, double *b, double *c) {
    const char *cursor = p;
    return model_obj_parse_double_token(&cursor, a) && model_obj_parse_double_token(&cursor, b) &&
           model_obj_parse_double_token(&cursor, c) && model_obj_line_tail_done(cursor);
}

/// @brief Clamp @p value into the [0, 1] range, mapping non-finite inputs to 0 (used for MTL
///   color/factor components that must stay normalized).
/// @param value Candidate MTL component.
/// @return The clamped value; 0.0 for NaN or infinity.
static double model_obj_clamp01(double value) {
    if (!isfinite(value))
        return 0.0;
    if (value < 0.0)
        return 0.0;
    if (value > 1.0)
        return 1.0;
    return value;
}

/// @brief Return the argument count for a Wavefront MTL texture-map option.
/// @details Counts are intentionally conservative for the common map options that appear before
///          filenames. Unknown options return -1 so the parser can stop treating the line as
///          option-only and preserve the remaining text as the texture path.
/// @param token Option token, including its leading '-'.
/// @return Number of required option arguments, -2 for a one-to-three numeric vector option,
///         or -1 when @p token is not recognized.
static int model_obj_map_option_arg_count(const char *token) {
    if (!token || token[0] != '-')
        return -1;
    if (strcmp(token, "-blendu") == 0 || strcmp(token, "-blendv") == 0 ||
        strcmp(token, "-boost") == 0 || strcmp(token, "-texres") == 0 ||
        strcmp(token, "-clamp") == 0 || strcmp(token, "-bm") == 0 ||
        strcmp(token, "-imfchan") == 0 || strcmp(token, "-type") == 0)
        return 1;
    if (strcmp(token, "-mm") == 0)
        return 2;
    if (strcmp(token, "-o") == 0 || strcmp(token, "-s") == 0 || strcmp(token, "-t") == 0)
        return -2;
    return -1;
}

/// @brief Return whether an MTL token looks like a numeric option argument.
/// @details Used only to keep variable-width options such as `-o`, `-s`, and `-t` from consuming
///          the filename. It accepts ordinary OBJ-style numeric starts, not full floating parsing.
/// @param start First byte of the token.
/// @param end One-past-end pointer for the token.
/// @return 1 when the token starts like a signed decimal/exponent number; otherwise 0.
static int model_obj_mtl_token_is_numeric(const char *start, const char *end) {
    if (!start || start >= end)
        return 0;
    if (*start == '+' || *start == '-')
        start++;
    return start < end && ((*start >= '0' && *start <= '9') || *start == '.');
}

/// @brief Advance over spaces and tabs within a trimmed MTL directive range.
/// @param p Current cursor.
/// @param end One-past-end pointer for the directive value.
/// @return First non-space/tab byte at or before @p end.
static const char *model_obj_skip_mtl_spaces(const char *p, const char *end) {
    while (p < end && (*p == ' ' || *p == '\t'))
        p++;
    return p;
}

/// @brief Advance over one whitespace-delimited MTL token.
/// @param p Token start.
/// @param end One-past-end pointer for the directive value.
/// @return First whitespace byte or @p end.
static const char *model_obj_skip_mtl_token(const char *p, const char *end) {
    while (p < end && *p != ' ' && *p != '\t')
        p++;
    return p;
}

/// @brief Extract the texture filename from an MTL map directive value.
/// @details Skips recognized `map_*` option tokens and their arguments, then returns the remaining
///          text as the path. Quoted paths have their quotes stripped; unquoted paths preserve
///          internal spaces so filenames such as `brick wall.png` survive option parsing.
/// @param value Nullable directive remainder containing options and a filename.
/// @return A malloc'd copy of the path, or NULL.
static char *model_obj_extract_map_path(const char *value) {
    const char *start;
    const char *end;
    if (!value)
        return NULL;
    start = value;
    while (*start == ' ' || *start == '\t')
        start++;
    end = start + strlen(start);
    while (end > start && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\n' || end[-1] == '\r'))
        end--;
    while (start < end && *start == '-') {
        const char *token_end = model_obj_skip_mtl_token(start, end);
        char option[32];
        size_t option_len = (size_t)(token_end - start);
        int arg_count;
        if (option_len == 0 || option_len >= sizeof(option))
            break;
        memcpy(option, start, option_len);
        option[option_len] = '\0';
        arg_count = model_obj_map_option_arg_count(option);
        if (arg_count == -1)
            break;
        start = token_end;
        if (arg_count == -2) {
            int consumed = 0;
            for (int i = 0; i < 3; i++) {
                const char *arg_start = model_obj_skip_mtl_spaces(start, end);
                const char *arg_end;
                if (arg_start >= end)
                    break;
                arg_end = model_obj_skip_mtl_token(arg_start, end);
                if (!model_obj_mtl_token_is_numeric(arg_start, arg_end))
                    break;
                start = arg_end;
                consumed++;
            }
            if (consumed == 0)
                break;
            start = model_obj_skip_mtl_spaces(start, end);
            continue;
        }
        for (int i = 0; i < arg_count; i++) {
            start = model_obj_skip_mtl_spaces(start, end);
            if (start >= end)
                return NULL;
            start = model_obj_skip_mtl_token(start, end);
        }
        start = model_obj_skip_mtl_spaces(start, end);
    }
    if (start >= end)
        return NULL;
    if (*start == '"' || *start == '\'') {
        char quote = *start;
        const char *quoted_start = start + 1;
        const char *quoted_end = quoted_start;
        while (quoted_end < end && *quoted_end != quote)
            quoted_end++;
        if (quoted_end < end)
            return model_strdup_range(quoted_start, quoted_end);
    } else {
        const char *scan = start;
        while (scan < end) {
            if (*scan == '"' || *scan == '\'') {
                char quote = *scan;
                const char *quoted_start = scan + 1;
                const char *quoted_end = quoted_start;
                while (quoted_end < end && *quoted_end != quote)
                    quoted_end++;
                if (quoted_end < end)
                    return model_strdup_range(quoted_start, quoted_end);
            }
            scan++;
        }
    }
    return model_strdup_range(start, end);
}
