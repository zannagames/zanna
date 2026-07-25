//===----------------------------------------------------------------------===//
// Part of the Zanna project, under the GNU GPL v3. See LICENSE.
// File: src/runtime/graphics/3d/rt_game3d_cache.inc  (#included from rt_game3d.c; not a standalone
// translation unit) Purpose: Game3D model-cache lifecycle: keys, residency estimation, victim
// selection and eviction
//===----------------------------------------------------------------------===//

#define game3d_asset_fseek(fp, off, whence) rt_file_stdio_seek64((fp), (off), (whence))
#define game3d_asset_ftell(fp) rt_file_stdio_tell64((fp))

/// @brief Return the template's loaded Model3D as a class-checked reference, or NULL
///   when the template is absent or its model slot holds the wrong type.
static void *game3d_model_template_model_ref(const rt_game3d_model_template *model_template) {
    return model_template ? rt_g3d_checked_or_null(model_template->model, RT_G3D_MODEL3D_CLASS_ID)
                          : NULL;
}

/// @brief Class-checked cast of an opaque GC handle to a ModelTemplate, or NULL on mismatch.
static rt_game3d_model_template *game3d_model_template_ref(void *obj) {
    return (rt_game3d_model_template *)rt_g3d_checked_or_null(
        obj, RT_G3D_GAME3D_MODEL_TEMPLATE_CLASS_ID);
}

/// @brief Return the asset handle's loaded entity as a class-checked reference, or NULL.
static void *game3d_asset_handle_entity_ref(const rt_game3d_asset_handle *handle) {
    return handle ? rt_g3d_checked_or_null(handle->entity, RT_G3D_GAME3D_ENTITY_CLASS_ID) : NULL;
}

/// @brief Return the asset handle's loaded ModelTemplate as a class-checked reference, or NULL.
static rt_game3d_model_template *game3d_asset_handle_template_ref(
    const rt_game3d_asset_handle *handle) {
    return handle ? game3d_model_template_ref(handle->model_template) : NULL;
}

/// @brief GC finalizer for a ModelTemplate: release the loaded model and the path string.
static void game3d_model_template_finalize(void *obj) {
    rt_game3d_model_template *model_template = (rt_game3d_model_template *)obj;
    if (!model_template)
        return;
    game3d_release_typed_ref(&model_template->model, RT_G3D_MODEL3D_CLASS_ID);
    game3d_release_ref((void **)&model_template->path);
}

/// @brief GC finalizer for AssetHandle3D: release terminal result and error text.
static void game3d_asset_handle_finalize(void *obj) {
    rt_game3d_asset_handle *handle = (rt_game3d_asset_handle *)obj;
    if (!handle)
        return;
    game3d_release_typed_ref(&handle->entity, RT_G3D_GAME3D_ENTITY_CLASS_ID);
    game3d_release_typed_ref(&handle->model_template, RT_G3D_GAME3D_MODEL_TEMPLATE_CLASS_ID);
    game3d_release_ref((void **)&handle->error);
    game3d_release_ref((void **)&handle->path);
}

/// @brief Ensure the global model cache can hold `need` entries, doubling and zeroing
///   new slots; returns 0 on allocation failure.
static int game3d_model_cache_grow(int32_t need) {
    int32_t new_cap;
    if (g_game3d_model_cache_capacity >= need)
        return 1;
    if (!game3d_compute_capacity(
            g_game3d_model_cache_capacity, need, 8, sizeof(rt_game3d_model_cache_entry), &new_cap))
        return 0;
    rt_game3d_model_cache_entry *grown = (rt_game3d_model_cache_entry *)realloc(
        g_game3d_model_cache, (size_t)new_cap * sizeof(*grown));
    if (!grown)
        return 0;
    memset(grown + g_game3d_model_cache_capacity,
           0,
           (size_t)(new_cap - g_game3d_model_cache_capacity) * sizeof(*grown));
    g_game3d_model_cache = grown;
    g_game3d_model_cache_capacity = new_cap;
    return 1;
}

/// @brief Compare two runtime strings for byte equality (NULLs treated as "").
static int game3d_string_equals(rt_string a, rt_string b) {
    const char *as = a ? rt_string_cstr(a) : "";
    const char *bs = b ? rt_string_cstr(b) : "";
    return as && bs && strcmp(as, bs) == 0;
}

/// @brief True when the runtime string contains at least one byte.
static int game3d_string_has_bytes(rt_string value) {
    const char *bytes = value ? rt_string_cstr(value) : "";
    return bytes && bytes[0] != '\0';
}

/// @brief Whether @p path ends in a supported 3D model extension
/// (.scene3d/.vscn/.gltf/.glb/.fbx/.obj/.stl), case-insensitive.
/// @details `.scene3d` is the canonical scene extension; `.vscn` remains an
///          accepted legacy alias (ADR 0182).
static int game3d_path_has_model_extension(const char *path) {
    static const char *const exts[] = {
        ".scene3d", ".vscn", ".gltf", ".glb", ".fbx", ".obj", ".stl"};
    if (!path)
        return 0;
    const char *dot = strrchr(path, '.');
    if (!dot || !dot[1])
        return 0;
    for (size_t i = 0; i < sizeof(exts) / sizeof(exts[0]); ++i) {
        const char *a = dot;
        const char *b = exts[i];
        while (*a && *b) {
            if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
                break;
            ++a;
            ++b;
        }
        if (*a == '\0' && *b == '\0')
            return 1;
    }
    return 0;
}

/// @brief Whether @p path ends in a glTF extension (.gltf/.glb), case-insensitive.
static int game3d_path_has_gltf_extension(const char *path) {
    static const char *const exts[] = {".gltf", ".glb"};
    if (!path)
        return 0;
    const char *dot = strrchr(path, '.');
    if (!dot || !dot[1])
        return 0;
    for (size_t i = 0; i < sizeof(exts) / sizeof(exts[0]); ++i) {
        const char *a = dot;
        const char *b = exts[i];
        while (*a && *b) {
            if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
                break;
            ++a;
            ++b;
        }
        if (*a == '\0' && *b == '\0')
            return 1;
    }
    return 0;
}

/// @brief Whether @p path ends in an FBX extension, case-insensitive.
static int game3d_path_has_fbx_extension(const char *path) {
    const char *dot = path ? strrchr(path, '.') : NULL;
    if (!dot || !dot[1])
        return 0;
    return tolower((unsigned char)dot[1]) == 'f' && tolower((unsigned char)dot[2]) == 'b' &&
           tolower((unsigned char)dot[3]) == 'x' && dot[4] == '\0';
}

/// @brief Read an entire file into a malloc'd buffer (caller frees; @p out_size set). NULL on
/// failure.
///
/// @details Uses 64-bit seek/tell helpers so large-file support is consistent
///   across Windows and POSIX builds. The root asset byte limit is still
///   enforced before allocation; this helper does not silently truncate files
///   whose length does not fit in @c long on 32-bit-long platforms.
///
/// @param path File-system path to read.
/// @param out_size Receives the number of bytes read on success.
/// @return A malloc-owned byte buffer, or NULL on open/seek/size/read failure.
static uint8_t *game3d_asset_read_file_bytes(const char *path, size_t *out_size) {
    FILE *file;
    int64_t len;
    uint8_t *data;
    size_t read_len;
    if (out_size)
        *out_size = 0;
    if (!path)
        return NULL;
    file = rt_file_stdio_open_utf8(path, "rb");
    if (!file)
        return NULL;
    if (game3d_asset_fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }
    len = (int64_t)game3d_asset_ftell(file);
    if (len < 0) {
        fclose(file);
        return NULL;
    }
    if ((uint64_t)len > RT_GAME3D_ASSET_ASYNC_ROOT_BYTES_MAX) {
        fclose(file);
        return NULL;
    }
    if (len == 0) {
        data = (uint8_t *)calloc(1u, 1u);
        fclose(file);
        return data;
    }
    if (game3d_asset_fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    data = (uint8_t *)malloc((size_t)len);
    if (!data) {
        fclose(file);
        return NULL;
    }
    read_len = fread(data, 1, (size_t)len, file);
    fclose(file);
    if (read_len != (size_t)len) {
        free(data);
        return NULL;
    }
    if (out_size)
        *out_size = read_len;
    return data;
}

/// @brief Load a model's root bytes, preferring the packed asset system and falling back to disk.
static GAME3D_UNUSED_PRIVATE uint8_t *game3d_asset_load_root_bytes(rt_string path,
                                                                   int8_t asset_path,
                                                                   size_t *out_size) {
    const char *path_cstr = path ? rt_string_cstr(path) : NULL;
    uint8_t *data;
    if (out_size)
        *out_size = 0;
    if (!path_cstr)
        return NULL;
    if (asset_path) {
        data = rt_asset_load_raw(path, out_size);
        if (data || strncmp(path_cstr, "asset://", 8) == 0)
            return data;
    }
    return game3d_asset_read_file_bytes(path_cstr, out_size);
}

/// @brief Saturating unsigned 64-bit addition (clamps to UINT64_MAX instead of wrapping).
static uint64_t game3d_u64_saturating_add(uint64_t a, uint64_t b) {
    if (UINT64_MAX - a < b)
        return UINT64_MAX;
    return a + b;
}

/// @brief Convert a uint64 to int64, saturating values above INT64_MAX.
static int64_t game3d_i64_from_u64_saturating(uint64_t value) {
    return value > (uint64_t)INT64_MAX ? INT64_MAX : (int64_t)value;
}

#ifdef ZANNA_ENABLE_GRAPHICS
/// @brief Convert an int64 to uint64, treating negative values as 0.
static uint64_t game3d_u64_from_i64_nonnegative(int64_t value) {
    return value > 0 ? (uint64_t)value : 0;
}

/// @brief Saturating unsigned 64-bit multiplication (clamps to UINT64_MAX on overflow).
static uint64_t game3d_u64_saturating_mul_u64(uint64_t a, uint64_t b) {
    if (a != 0 && b > UINT64_MAX / a)
        return UINT64_MAX;
    return a * b;
}

typedef struct {
    void **items;
    int32_t count;
    int32_t capacity;
    int8_t overflowed;
} game3d_seen_ptr_set;

/// @brief Initialize a temporary pointer-deduplication set to an empty state.
/// @details The set owns only the pointer array it allocates; it never retains or
///   releases the objects whose addresses are recorded.
static void game3d_seen_ptr_set_init(game3d_seen_ptr_set *set) {
    if (set)
        memset(set, 0, sizeof(*set));
}

/// @brief Release the temporary storage owned by a pointer-deduplication set.
/// @details Safe to call on an already-empty set; recorded object pointers are
///   bookkeeping only and are not released.
static void game3d_seen_ptr_set_free(game3d_seen_ptr_set *set) {
    if (!set)
        return;
    free(set->items);
    memset(set, 0, sizeof(*set));
}

/// @brief Whether @p value is already in the pointer set.
static int game3d_seen_ptr_set_contains(const game3d_seen_ptr_set *set, void *value) {
    if (!value)
        return 1;
    if (!set)
        return 0;
    for (int32_t i = 0; i < set->count; ++i) {
        if (set->items[i] == value)
            return 1;
    }
    return 0;
}

/// @brief Mark a pointer as seen. Returns 1 for a newly-seen pointer, 0 for duplicates/null.
/// @details If the dedupe set cannot grow, stay conservative by counting this and later resources
///          instead of undercounting resident memory.
static int game3d_seen_ptr_set_mark(game3d_seen_ptr_set *set, void *value) {
    void **grown;
    int32_t new_capacity;
    if (!value)
        return 0;
    if (!set || set->overflowed)
        return 1;
    if (game3d_seen_ptr_set_contains(set, value))
        return 0;
    if (set->count >= set->capacity) {
        new_capacity = set->capacity > 0 ? set->capacity * 2 : 64;
        if (new_capacity <= set->capacity ||
            (size_t)new_capacity > SIZE_MAX / sizeof(*set->items)) {
            set->overflowed = 1;
            return 1;
        }
        grown = (void **)realloc(set->items, (size_t)new_capacity * sizeof(*grown));
        if (!grown) {
            set->overflowed = 1;
            return 1;
        }
        set->items = grown;
        set->capacity = new_capacity;
    }
    set->items[set->count++] = value;
    return 1;
}

/// @brief Estimate the resident memory of a Pixels object (width × height × 4 bytes).
static uint64_t game3d_pixels_estimate_resident_bytes(void *pixels) {
    rt_pixels_impl *impl;
    uint64_t width;
    uint64_t height;
    uint64_t texels;

    impl = rt_pixels_checked_impl_or_null(pixels);
    if (!impl)
        return 0;
    width = game3d_u64_from_i64_nonnegative(impl->width);
    height = game3d_u64_from_i64_nonnegative(impl->height);
    texels = game3d_u64_saturating_mul_u64(width, height);
    return game3d_u64_saturating_mul_u64(texels, 4u);
}

/// @brief Add a texture's resident bytes to the total, but only the first time it is seen.
/// @details Records the texture in the seen-set so a texture shared by several materials is counted
/// once.
static uint64_t game3d_count_material_texture_once(void *texture,
                                                   game3d_seen_ptr_set *seen_textures) {
    int is_texture_asset;
    if (!texture || !seen_textures)
        return 0;
    is_texture_asset = rt_g3d_has_class(texture, RT_G3D_TEXTUREASSET3D_CLASS_ID);
    if (!is_texture_asset && !rt_pixels_checked_impl_or_null(texture))
        return 0;
    if (!game3d_seen_ptr_set_mark(seen_textures, texture))
        return 0;
    if (is_texture_asset)
        return game3d_u64_from_i64_nonnegative(rt_textureasset3d_get_retained_bytes(texture));
    return game3d_pixels_estimate_resident_bytes(texture);
}

/// @brief Add a cubemap's resident bytes to the total once.
/// @details Counts the cubemap object and each complete face payload while using
///   the same seen-set as material textures, so shared face images or shared
///   cubemaps do not inflate cache residency estimates.
static uint64_t game3d_count_material_cubemap_once(void *cubemap,
                                                   game3d_seen_ptr_set *seen_textures) {
#ifdef ZANNA_ENABLE_GRAPHICS
    rt_cubemap3d *cm = (rt_cubemap3d *)cubemap;
    uint64_t total = 0;
    if (!cubemap || !seen_textures || !rt_g3d_has_class(cubemap, RT_G3D_CUBEMAP3D_CLASS_ID) ||
        !rt_cubemap3d_is_complete(cubemap))
        return 0;
    if (!game3d_seen_ptr_set_mark(seen_textures, cubemap))
        return 0;
    total = game3d_u64_saturating_add(total, (uint64_t)sizeof(*cm));
    for (int face = 0; face < 6; ++face) {
        if (!cm->faces[face])
            continue;
        if (!game3d_seen_ptr_set_mark(seen_textures, cm->faces[face]))
            continue;
        total = game3d_u64_saturating_add(total,
                                          game3d_pixels_estimate_resident_bytes(cm->faces[face]));
    }
    return total;
#else
    (void)cubemap;
    (void)seen_textures;
    return 0;
#endif
}

/// @brief Estimate the resident texture bytes of a material across all its texture slots (deduped).
static uint64_t game3d_material_estimate_texture_bytes(rt_material3d *material,
                                                       game3d_seen_ptr_set *seen_textures) {
    uint64_t total = 0;
    if (!material || !rt_g3d_has_class(material, RT_G3D_MATERIAL3D_CLASS_ID))
        return 0;
    total = game3d_u64_saturating_add(
        total, game3d_count_material_texture_once(material->texture, seen_textures));
    total = game3d_u64_saturating_add(
        total, game3d_count_material_texture_once(material->normal_map, seen_textures));
    total = game3d_u64_saturating_add(
        total, game3d_count_material_texture_once(material->specular_map, seen_textures));
    total = game3d_u64_saturating_add(
        total, game3d_count_material_texture_once(material->emissive_map, seen_textures));
    total = game3d_u64_saturating_add(
        total, game3d_count_material_texture_once(material->metallic_roughness_map, seen_textures));
    total = game3d_u64_saturating_add(
        total, game3d_count_material_texture_once(material->ao_map, seen_textures));
    total = game3d_u64_saturating_add(
        total, game3d_count_material_cubemap_once(material->env_map, seen_textures));
    return total;
}

/// @brief Conservative resident-byte estimate for cache policy decisions.
static uint64_t game3d_model_template_estimate_resident_bytes(
    rt_game3d_model_template *model_template) {
    game3d_seen_ptr_set seen_textures;
    uint64_t total = sizeof(*model_template);
    if (!model_template)
        return 1;
    game3d_seen_ptr_set_init(&seen_textures);
    if (model_template->path) {
        const char *path = rt_string_cstr(model_template->path);
        if (path)
            total = game3d_u64_saturating_add(total, (uint64_t)strlen(path) + 1u);
    }
    void *model = game3d_model_template_model_ref(model_template);
    if (model) {
        int64_t mesh_count = rt_model3d_get_mesh_count(model);
        int64_t material_count = rt_model3d_get_material_count(model);
        int64_t skeleton_count = rt_model3d_get_skeleton_count(model);
        int64_t animation_count = rt_model3d_get_animation_count(model);
        int64_t node_animation_count = rt_model3d_get_node_animation_count(model);
        int64_t node_count = rt_model3d_get_node_count(model);
        total = game3d_u64_saturating_add(total, 256u);
        total = game3d_u64_saturating_add(
            total,
            game3d_u64_saturating_mul_u64(game3d_u64_from_i64_nonnegative(material_count), 256u));
        total = game3d_u64_saturating_add(
            total,
            game3d_u64_saturating_mul_u64(game3d_u64_from_i64_nonnegative(skeleton_count), 1024u));
        total = game3d_u64_saturating_add(
            total,
            game3d_u64_saturating_mul_u64(game3d_u64_from_i64_nonnegative(animation_count), 2048u));
        total = game3d_u64_saturating_add(
            total,
            game3d_u64_saturating_mul_u64(game3d_u64_from_i64_nonnegative(node_animation_count),
                                          2048u));
        total = game3d_u64_saturating_add(
            total,
            game3d_u64_saturating_mul_u64(game3d_u64_from_i64_nonnegative(node_count), 256u));
        for (int64_t i = 0; i < material_count; ++i) {
            rt_material3d *material = (rt_material3d *)rt_model3d_get_material(model, i);
            total = game3d_u64_saturating_add(
                total, game3d_material_estimate_texture_bytes(material, &seen_textures));
        }
        for (int64_t i = 0; i < mesh_count; ++i) {
            void *mesh = rt_model3d_get_mesh(model, i);
            if (rt_g3d_has_class(mesh, RT_G3D_MESH3D_CLASS_ID)) {
                total = game3d_u64_saturating_add(
                    total, game3d_u64_from_i64_nonnegative(rt_mesh3d_get_resident_bytes(mesh)));
            }
        }
    }
    game3d_seen_ptr_set_free(&seen_textures);
    return total > 0 ? total : 1;
}

/// @brief Estimate a mesh's resident memory from its vertex and index buffer sizes.
static uint64_t game3d_mesh_estimate_resident_bytes(void *mesh) {
    if (!mesh || !rt_g3d_has_class(mesh, RT_G3D_MESH3D_CLASS_ID))
        return 0;
    return game3d_u64_from_i64_nonnegative(rt_mesh3d_get_resident_bytes(mesh));
}

/// @brief Add a mesh's resident bytes to the total only the first time it is seen (dedup).
static uint64_t game3d_count_scene_mesh_once(void *mesh, game3d_seen_ptr_set *seen_meshes) {
    if (!mesh || !seen_meshes)
        return 0;
    if (!rt_g3d_has_class(mesh, RT_G3D_MESH3D_CLASS_ID))
        return 0;
    if (!game3d_seen_ptr_set_mark(seen_meshes, mesh))
        return 0;
    return game3d_mesh_estimate_resident_bytes(mesh);
}

/// @brief Add a material's resident texture bytes to the total only the first time it is seen
/// (dedup).
static uint64_t game3d_count_scene_material_once(rt_material3d *material,
                                                 game3d_seen_ptr_set *seen_materials,
                                                 game3d_seen_ptr_set *seen_textures) {
    uint64_t bytes;

    if (!material || !seen_materials)
        return 0;
    if (!rt_g3d_has_class(material, RT_G3D_MATERIAL3D_CLASS_ID))
        return 0;
    if (!game3d_seen_ptr_set_mark(seen_materials, material))
        return 0;
    bytes = 256u;
    bytes = game3d_u64_saturating_add(
        bytes, game3d_material_estimate_texture_bytes(material, seen_textures));
    return bytes;
}

/// @brief Bound a private array count by its backing pointer and capacity before direct reads.
static int32_t game3d_clamped_array_count(const void *array, int32_t count, int32_t capacity) {
    if (!array || count <= 0 || capacity <= 0)
        return 0;
    return count < capacity ? count : capacity;
}

/// @brief Safe LOD count for direct SceneNode3D residency traversal.
static int32_t game3d_scene_node_lod_count(const rt_scene_node3d *node) {
    return node ? game3d_clamped_array_count(node->lod_levels, node->lod_count, node->lod_capacity)
                : 0;
}

/// @brief Safe child count for direct SceneNode3D residency traversal, capped by scene size.
static int32_t game3d_scene_node_child_count(const rt_scene_node3d *node,
                                             int32_t scene_node_bound) {
    int32_t count;
    if (scene_node_bound <= 0)
        scene_node_bound = 1;
    count =
        node ? game3d_clamped_array_count(node->children, node->child_count, node->child_capacity)
             : 0;
    return count < scene_node_bound ? count : scene_node_bound;
}

typedef struct {
    game3d_seen_ptr_set seen_meshes;
    game3d_seen_ptr_set seen_materials;
    game3d_seen_ptr_set seen_textures;
} game3d_scene_residency_context;

static void game3d_scene_residency_context_init(game3d_scene_residency_context *ctx) {
    if (!ctx)
        return;
    game3d_seen_ptr_set_init(&ctx->seen_meshes);
    game3d_seen_ptr_set_init(&ctx->seen_materials);
    game3d_seen_ptr_set_init(&ctx->seen_textures);
}

static void game3d_scene_residency_context_free(game3d_scene_residency_context *ctx) {
    if (!ctx)
        return;
    game3d_seen_ptr_set_free(&ctx->seen_meshes);
    game3d_seen_ptr_set_free(&ctx->seen_materials);
    game3d_seen_ptr_set_free(&ctx->seen_textures);
}

static uint64_t game3d_scene_count_node_resources(rt_scene_node3d *node,
                                                  game3d_scene_residency_context *ctx) {
    uint64_t total = (uint64_t)sizeof(*node);
    int32_t lod_count = game3d_scene_node_lod_count(node);
    total = game3d_u64_saturating_add(total,
                                      game3d_count_scene_mesh_once(node->mesh, &ctx->seen_meshes));
    for (int32_t i = 0; i < lod_count; ++i) {
        total = game3d_u64_saturating_add(
            total, game3d_count_scene_mesh_once(node->lod_levels[i].mesh, &ctx->seen_meshes));
    }
    if (node->has_impostor) {
        total = game3d_u64_saturating_add(
            total, game3d_count_scene_mesh_once(node->impostor_mesh, &ctx->seen_meshes));
        total = game3d_u64_saturating_add(
            total,
            game3d_count_scene_material_once((rt_material3d *)node->impostor_material,
                                             &ctx->seen_materials,
                                             &ctx->seen_textures));
    }
    total = game3d_u64_saturating_add(
        total,
        game3d_count_scene_material_once(
            (rt_material3d *)node->material, &ctx->seen_materials, &ctx->seen_textures));
    return total;
}

static int game3d_scene_stack_push(rt_scene_node3d ***stack_io,
                                   size_t *count_io,
                                   size_t *capacity_io,
                                   rt_scene_node3d *child) {
    child = (rt_scene_node3d *)rt_g3d_checked_or_null(child, RT_G3D_SCENENODE3D_CLASS_ID);
    if (!child)
        return 1;
    rt_scene_node3d **stack = *stack_io;
    size_t count = *count_io;
    size_t capacity = *capacity_io;
    if (count >= capacity) {
        size_t new_capacity = capacity * 2u;
        rt_scene_node3d **grown;
        if (new_capacity <= capacity || new_capacity > SIZE_MAX / sizeof(rt_scene_node3d *))
            return 0;
        grown = (rt_scene_node3d **)realloc(stack, new_capacity * sizeof(rt_scene_node3d *));
        if (!grown)
            return 0;
        stack = grown;
        capacity = new_capacity;
    }
    stack[count++] = child;
    *stack_io = stack;
    *count_io = count;
    *capacity_io = capacity;
    return 1;
}

/// @brief Estimate resident memory reachable from one SceneNode3D hierarchy root.
/// @details Walks at most @p node_bound validated nodes and deduplicates meshes, materials, and
///   textures shared anywhere in the hierarchy. Invalid child slots are ignored. Allocation
///   failure returns the conservative total accumulated so far rather than affecting streaming.
/// @param root_obj Borrowed SceneNode3D root to inspect.
/// @param node_bound Positive traversal ceiling used to contain corrupt child graphs.
/// @return Saturating estimate of resident node and render-resource bytes.
static uint64_t game3d_scene_root_estimate_resident_bytes(void *root_obj, int32_t node_bound) {
    rt_scene_node3d *root =
        (rt_scene_node3d *)rt_g3d_checked_or_null(root_obj, RT_G3D_SCENENODE3D_CLASS_ID);
    rt_scene_node3d **stack = NULL;
    size_t count = 0;
    size_t capacity = 0;
    game3d_scene_residency_context residency;
    int32_t visited_count = 0;
    uint64_t total = 0;

    if (!root)
        return 0;
    if (node_bound <= 0)
        node_bound = 1;
    if ((uint32_t)node_bound > RT_SCENE3D_MAX_OWNER_TRANSACTION_NODES)
        node_bound = (int32_t)RT_SCENE3D_MAX_OWNER_TRANSACTION_NODES;
    game3d_scene_residency_context_init(&residency);

    capacity = 32u;
    stack = (rt_scene_node3d **)malloc(capacity * sizeof(*stack));
    if (!stack)
        return 0;
    stack[count++] = root;

    while (count > 0) {
        rt_scene_node3d *node = stack[--count];
        int32_t child_count;
        if (!node)
            continue;
        if (++visited_count > node_bound)
            break;

        total =
            game3d_u64_saturating_add(total, game3d_scene_count_node_resources(node, &residency));

        child_count = game3d_scene_node_child_count(node, node_bound - visited_count + 1);
        for (int32_t i = 0; i < child_count; ++i) {
            if (!game3d_scene_stack_push(&stack, &count, &capacity, node->children[i])) {
                free(stack);
                game3d_scene_residency_context_free(&residency);
                return total;
            }
        }
    }
    free(stack);
    game3d_scene_residency_context_free(&residency);
    return total;
}

/// @brief Estimate a Scene3D's resident hierarchy bytes using its validated cached node bound.
/// @param scene_obj Borrowed Scene3D handle whose current root should be inspected.
/// @return Saturating resident-byte estimate, or zero for an invalid/empty scene.
static uint64_t game3d_scene_estimate_resident_bytes(void *scene_obj) {
    rt_scene3d *scene = (rt_scene3d *)rt_g3d_checked_or_null(scene_obj, RT_G3D_SCENE3D_CLASS_ID);
    int32_t node_bound;
    if (!scene)
        return 0;
    node_bound = scene->node_count > 0 ? scene->node_count : 1;
    return game3d_scene_root_estimate_resident_bytes(scene->root, node_bound);
}

/// @brief Estimate resident bytes owned by an Entity3D's detached scene-node hierarchy.
/// @details Streamed scenes transfer their former implicit root into an entity before spawning.
///   This estimator therefore follows the entity-owned root rather than the now-empty source
///   Scene3D. The global owner-transaction ceiling bounds malformed or cyclic private state.
/// @param entity_obj Borrowed Entity3D handle to inspect.
/// @return Saturating resident-byte estimate, or zero for an invalid entity/node.
static uint64_t game3d_entity_estimate_resident_bytes(void *entity_obj) {
    rt_game3d_entity *entity =
        (rt_game3d_entity *)rt_g3d_checked_or_null(entity_obj, RT_G3D_GAME3D_ENTITY_CLASS_ID);
    return game3d_scene_root_estimate_resident_bytes(
        game3d_entity_node_ref(entity), (int32_t)RT_SCENE3D_MAX_OWNER_TRANSACTION_NODES);
}
#else
/// @brief Conservative resident-byte estimate for cache policy decisions in headless builds.
static uint64_t game3d_model_template_estimate_resident_bytes(
    rt_game3d_model_template *model_template) {
    uint64_t total = sizeof(*model_template);
    if (!model_template)
        return 1;
    if (model_template->path) {
        const char *path = rt_string_cstr(model_template->path);
        if (path)
            total = game3d_u64_saturating_add(total, (uint64_t)strlen(path) + 1u);
    }
    return total > 0 ? total : 1;
}

/// @brief Scene residency is unavailable without the private graphics runtime layouts.
static uint64_t game3d_scene_estimate_resident_bytes(void *scene_obj) {
    (void)scene_obj;
    return 0;
}

/// @brief Entity hierarchy residency is unavailable without private graphics runtime layouts.
/// @param entity_obj Ignored Entity3D handle retained for source-compatible streaming policy.
/// @return Always zero so callers use their authored conservative fallback.
static uint64_t game3d_entity_estimate_resident_bytes(void *entity_obj) {
    (void)entity_obj;
    return 0;
}
#endif

/// @brief Return the next monotonic model-cache access tick (for LRU recency ordering).
static uint64_t game3d_model_cache_next_tick(void) {
    if (g_game3d_model_cache_tick == UINT64_MAX) {
        for (int32_t i = 0; i < g_game3d_model_cache_count; ++i)
            g_game3d_model_cache[i].last_used = 1;
        g_game3d_model_cache_tick = 1;
    }
    return ++g_game3d_model_cache_tick;
}

/// @brief Read the current model-cache generation counter (bumped to invalidate all entries at
/// once).
static uint64_t game3d_model_cache_current_generation(void) {
    uint64_t generation;
    game3d_model_cache_lock();
    generation = g_game3d_model_cache_generation;
    game3d_model_cache_unlock();
    return generation;
}

/// @brief Whether @p generation still equals the current cache generation (i.e. not invalidated).
static int game3d_model_cache_generation_matches(uint64_t generation) {
    int matches;
    game3d_model_cache_lock();
    matches = g_game3d_model_cache_generation == generation;
    game3d_model_cache_unlock();
    return matches;
}

/// @brief Bump the cache generation, logically invalidating all existing entries (lock held).
static void game3d_model_cache_advance_generation_locked(void) {
    if (g_game3d_model_cache_generation == UINT64_MAX)
        g_game3d_model_cache_generation = 1;
    else
        g_game3d_model_cache_generation++;
}

static rt_string game3d_model_cache_key_asset_path(rt_string path) {
    const char *raw = path ? rt_string_cstr(path) : "";
    char normalized[RT_GAME3D_MODEL_CACHE_KEY_MAX];
    char **segments = NULL;
    int32_t segment_capacity = 0;
    int32_t segment_count = 0;
    size_t out_len = 0;
    const char *p;
    rt_string result;
    if (!raw)
        raw = "";
    p = strncmp(raw, "asset://", 8) == 0 ? raw + 8 : raw;
    while (*p == '/' || *p == '\\')
        p++;
    memcpy(normalized, "asset://", 8u);
    out_len = 8u;
    while (*p) {
        const char *start;
        size_t len;
        while (*p == '/' || *p == '\\')
            p++;
        start = p;
        while (*p && *p != '/' && *p != '\\')
            p++;
        len = (size_t)(p - start);
        if (len == 0 || (len == 1 && start[0] == '.'))
            continue;
        if (len == 2 && start[0] == '.' && start[1] == '.') {
            if (segment_count > 0) {
                segment_count--;
                out_len = (size_t)(segments[segment_count] - normalized);
                normalized[out_len] = '\0';
            }
            continue;
        }
        if (out_len + (out_len > 8u ? 1u : 0u) + len >= sizeof(normalized)) {
            free(segments);
            return rt_string_ref(path ? path : rt_const_cstr(""));
        }
        if (segment_count >= segment_capacity) {
            int32_t new_capacity = segment_capacity <= 0 ? 16 : segment_capacity * 2;
            char **grown;
            if (new_capacity < segment_capacity ||
                (size_t)new_capacity > SIZE_MAX / sizeof(char *)) {
                free(segments);
                return rt_string_ref(path ? path : rt_const_cstr(""));
            }
            grown = (char **)realloc(segments, (size_t)new_capacity * sizeof(char *));
            if (!grown) {
                free(segments);
                return rt_string_ref(path ? path : rt_const_cstr(""));
            }
            segments = grown;
            segment_capacity = new_capacity;
        }
        if (out_len > 8u)
            normalized[out_len++] = '/';
        segments[segment_count++] = normalized + out_len;
        memcpy(normalized + out_len, start, len);
        out_len += len;
        normalized[out_len] = '\0';
    }
    result = rt_string_from_bytes(normalized, out_len);
    free(segments);
    return result;
}

/// @brief Build the stable cache key for a model path.
static rt_string game3d_model_cache_key_path(rt_string path, int8_t asset_path) {
    const char *raw = path ? rt_string_cstr(path) : "";
    if (!raw)
        raw = "";
    if (asset_path)
        return game3d_model_cache_key_asset_path(path);
    rt_string absolute = rt_path_abs(path ? path : rt_const_cstr(""));
    if (absolute)
        return absolute;
    return rt_string_ref(path ? path : rt_const_cstr(""));
}

/// @brief Look up a model-cache entry by path + asset flag; -1 if absent.
static int32_t game3d_model_cache_find_index(rt_string path, int8_t asset_path) {
    for (int32_t i = 0; i < g_game3d_model_cache_count; ++i) {
        rt_game3d_model_cache_entry *entry = &g_game3d_model_cache[i];
        if (entry->asset_path == asset_path && game3d_string_equals(entry->path, path))
            return i;
    }
    return -1;
}

/// @brief Sanitize a residency priority value (finite, clamped) for cache victim ordering.
static double game3d_model_cache_sanitize_priority(double priority) {
    if (!isfinite(priority))
        return 0.0;
    if (priority > 1000000000.0)
        return 1000000000.0;
    if (priority < -1000000000.0)
        return -1000000000.0;
    return priority;
}

/// @brief Sanitize a residency distance value (finite, non-negative) for cache victim ordering.
static double game3d_model_cache_sanitize_distance(double distance) {
    if (!isfinite(distance) || distance < 0.0)
        return DBL_MAX;
    if (distance > DBL_MAX)
        return DBL_MAX;
    return distance;
}

/// @brief Reset a cache entry's residency hint (priority/distance) to the neutral defaults.
static void game3d_model_cache_set_default_residency_hint(rt_game3d_model_cache_entry *entry) {
    if (!entry)
        return;
    entry->residency_priority = 0.0;
    entry->residency_distance = DBL_MAX;
}

/// @brief Whether @p candidate is a better (worse-to-keep) eviction victim than @p current.
/// @details Ranks by lower residency priority, then greater distance, then least-recently used;
///          loading entries are never chosen.
static int game3d_model_cache_entry_is_worse_victim(const rt_game3d_model_cache_entry *candidate,
                                                    const rt_game3d_model_cache_entry *current) {
    if (!candidate || candidate->loading)
        return 0;
    if (!current)
        return 1;
    if (candidate->residency_priority < current->residency_priority)
        return 1;
    if (candidate->residency_priority > current->residency_priority)
        return 0;
    if (candidate->residency_distance > current->residency_distance)
        return 1;
    if (candidate->residency_distance < current->residency_distance)
        return 0;
    return candidate->last_used < current->last_used;
}

typedef struct {
    rt_string path;
    void *model_template;
    int8_t asset_path;
    double residency_priority;
    double residency_distance;
    uint64_t last_used;
    int32_t original_index;
} game3d_model_cache_victim_candidate_t;

static game3d_model_cache_victim_candidate_t *g_game3d_model_cache_victim_heap = NULL;
static int32_t g_game3d_model_cache_victim_heap_capacity = 0;

/// @brief Ensure the reusable victim heap can hold @p needed entries.
/// @details The model cache lock must be held. Reusing this scratch buffer avoids an allocation
///          spike each time byte-budget enforcement runs.
/// @param needed Maximum number of victim candidates to stage.
/// @return Scratch heap storage, or NULL if allocation failed.
static game3d_model_cache_victim_candidate_t *game3d_model_cache_acquire_victim_heap(
    int32_t needed) {
    game3d_model_cache_victim_candidate_t *grown;
    if (needed <= 0)
        return NULL;
    if (g_game3d_model_cache_victim_heap_capacity >= needed)
        return g_game3d_model_cache_victim_heap;
    if ((size_t)needed > SIZE_MAX / sizeof(*g_game3d_model_cache_victim_heap))
        return NULL;
    grown = (game3d_model_cache_victim_candidate_t *)realloc(g_game3d_model_cache_victim_heap,
                                                             (size_t)needed * sizeof(*grown));
    if (!grown)
        return NULL;
    g_game3d_model_cache_victim_heap = grown;
    g_game3d_model_cache_victim_heap_capacity = needed;
    return g_game3d_model_cache_victim_heap;
}

/// @brief Eviction ordering for model-cache victims: non-zero if @p candidate is a better
///   eviction target than @p current — lower residency priority first, then greater residency
///   distance, then least-recently-used, then lower original index as a stable tiebreak.
static int game3d_model_cache_victim_candidate_is_worse(
    const game3d_model_cache_victim_candidate_t *candidate,
    const game3d_model_cache_victim_candidate_t *current) {
    if (!candidate)
        return 0;
    if (!current)
        return 1;
    if (candidate->residency_priority < current->residency_priority)
        return 1;
    if (candidate->residency_priority > current->residency_priority)
        return 0;
    if (candidate->residency_distance > current->residency_distance)
        return 1;
    if (candidate->residency_distance < current->residency_distance)
        return 0;
    if (candidate->last_used < current->last_used)
        return 1;
    if (candidate->last_used > current->last_used)
        return 0;
    return candidate->original_index < current->original_index;
}

/// @brief Swap two model-cache victim-heap entries.
static void game3d_model_cache_victim_heap_swap(game3d_model_cache_victim_candidate_t *a,
                                                game3d_model_cache_victim_candidate_t *b) {
    game3d_model_cache_victim_candidate_t tmp = *a;
    *a = *b;
    *b = tmp;
}

/// @brief Sift a victim-heap entry down to restore heap order (best eviction target at the
///   root), comparing children with game3d_model_cache_victim_candidate_is_worse.
static void game3d_model_cache_victim_heap_sift_down(game3d_model_cache_victim_candidate_t *heap,
                                                     int32_t count,
                                                     int32_t index) {
    while (heap && index >= 0) {
        int32_t left = index * 2 + 1;
        int32_t right = left + 1;
        int32_t best = index;
        if (left < count && game3d_model_cache_victim_candidate_is_worse(&heap[left], &heap[best]))
            best = left;
        if (right < count &&
            game3d_model_cache_victim_candidate_is_worse(&heap[right], &heap[best]))
            best = right;
        if (best == index)
            break;
        game3d_model_cache_victim_heap_swap(&heap[index], &heap[best]);
        index = best;
    }
}

/// @brief Snapshot all non-loading model-cache entries into @p heap (up to @p heap_capacity)
///   and heapify so the best eviction victim is at the root.
/// @return The number of candidate entries collected.
static int32_t game3d_model_cache_build_victim_heap(game3d_model_cache_victim_candidate_t *heap,
                                                    int32_t heap_capacity) {
    int32_t count = 0;
    if (!heap || heap_capacity <= 0)
        return 0;
    for (int32_t i = 0; i < g_game3d_model_cache_count && count < heap_capacity; ++i) {
        rt_game3d_model_cache_entry *entry = &g_game3d_model_cache[i];
        if (entry->loading)
            continue;
        rt_game3d_model_template *ready = game3d_model_template_ref(entry->model_template);
        if (!ready)
            continue;
        heap[count].path = entry->path;
        heap[count].model_template = ready;
        heap[count].asset_path = entry->asset_path;
        heap[count].residency_priority = entry->residency_priority;
        heap[count].residency_distance = entry->residency_distance;
        heap[count].last_used = entry->last_used;
        heap[count].original_index = i;
        count++;
    }
    for (int32_t i = count / 2 - 1; i >= 0; --i)
        game3d_model_cache_victim_heap_sift_down(heap, count, i);
    return count;
}

/// @brief Re-resolve the live model-cache index matching @p candidate (by asset path, model
///   template, and path), since the heap holds copies; -1 if it is no longer present.
static int32_t game3d_model_cache_find_victim_candidate(
    const game3d_model_cache_victim_candidate_t *candidate) {
    if (!candidate)
        return -1;
    for (int32_t i = 0; i < g_game3d_model_cache_count; ++i) {
        rt_game3d_model_cache_entry *entry = &g_game3d_model_cache[i];
        if (entry->loading)
            continue;
        if (entry->asset_path == candidate->asset_path &&
            entry->model_template == candidate->model_template &&
            game3d_string_equals(entry->path, candidate->path))
            return i;
    }
    return -1;
}

/// @brief Select the index of the best eviction victim in the model cache (-1 if none evictable).
static int32_t game3d_model_cache_select_victim(void) {
    int32_t victim = -1;
    const rt_game3d_model_cache_entry *victim_entry = NULL;
    for (int32_t i = 0; i < g_game3d_model_cache_count; ++i) {
        rt_game3d_model_cache_entry *entry = &g_game3d_model_cache[i];
        if (game3d_model_cache_entry_is_worse_victim(entry, victim_entry)) {
            victim = i;
            victim_entry = entry;
        }
    }
    return victim;
}

/// @brief Drop one cache entry and compact the array.
static void game3d_model_cache_remove_at(int32_t index) {
    if (index < 0 || index >= g_game3d_model_cache_count)
        return;
    if (g_game3d_model_cache[index].resident_bytes > g_game3d_model_resident_bytes)
        g_game3d_model_resident_bytes = 0;
    else
        g_game3d_model_resident_bytes -= g_game3d_model_cache[index].resident_bytes;
    game3d_release_ref((void **)&g_game3d_model_cache[index].path);
    game3d_release_typed_ref(&g_game3d_model_cache[index].model_template,
                             RT_G3D_GAME3D_MODEL_TEMPLATE_CLASS_ID);
    for (int32_t i = index; i < g_game3d_model_cache_count - 1; ++i)
        g_game3d_model_cache[i] = g_game3d_model_cache[i + 1];
    g_game3d_model_cache_count--;
    memset(&g_game3d_model_cache[g_game3d_model_cache_count], 0, sizeof(g_game3d_model_cache[0]));
}

/// @brief Enforce the fixed model-cache entry cap by evicting the least-important entry.
static void game3d_model_cache_evict_if_full(void) {
    if (g_game3d_model_cache_count < RT_GAME3D_MODEL_CACHE_MAX_ENTRIES)
        return;
    int32_t victim = game3d_model_cache_select_victim();
    if (victim >= 0)
        game3d_model_cache_remove_at(victim);
}

/// @brief Evict least-important non-loading entries until the byte budget is met.
static void game3d_model_cache_evict_to_budget(void) {
    game3d_model_cache_victim_candidate_t *heap;
    int32_t heap_count;
    if (g_game3d_model_resident_bytes <= g_game3d_model_residency_budget_bytes)
        return;
    if (g_game3d_model_cache_count <= 0)
        return;
    heap = game3d_model_cache_acquire_victim_heap(g_game3d_model_cache_count);
    if (!heap)
        goto fallback_scan;
    heap_count = game3d_model_cache_build_victim_heap(heap, g_game3d_model_cache_count);
    while (heap_count > 0 &&
           g_game3d_model_resident_bytes > g_game3d_model_residency_budget_bytes) {
        game3d_model_cache_victim_candidate_t victim = heap[0];
        int32_t index;
        heap[0] = heap[--heap_count];
        game3d_model_cache_victim_heap_sift_down(heap, heap_count, 0);
        index = game3d_model_cache_find_victim_candidate(&victim);
        if (index >= 0)
            game3d_model_cache_remove_at(index);
    }
    return;

fallback_scan:
    while (g_game3d_model_resident_bytes > g_game3d_model_residency_budget_bytes) {
        int32_t victim = game3d_model_cache_select_victim();
        if (victim < 0)
            break;
        game3d_model_cache_remove_at(victim);
    }
}

/// @brief Remove a cached template by object identity, preserving external refs.
static int game3d_model_cache_evict_template(void *model_template) {
    if (!model_template)
        return 0;
    for (int32_t i = 0; i < g_game3d_model_cache_count; ++i) {
        rt_game3d_model_cache_entry *entry = &g_game3d_model_cache[i];
        if (!entry->loading && entry->model_template == model_template) {
            game3d_model_cache_remove_at(i);
            return 1;
        }
    }
    return 0;
}
