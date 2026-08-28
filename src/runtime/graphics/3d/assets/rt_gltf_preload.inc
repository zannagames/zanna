//===----------------------------------------------------------------------===//
// Part of the Zanna project, under the GNU GPL v3. See LICENSE.
// File: src/runtime/graphics/3d/assets/rt_gltf_preload.inc  (#included from rt_gltf.c; not a
// standalone translation unit) Purpose: glTF preload-bundle staging (buffers, views, accessors,
// meshes, morphs, images)
//===----------------------------------------------------------------------===//

/**
 * @file rt_gltf_preload.inc
 * @brief Stages glTF buffers, meshes, morph targets, and images for asynchronous loading.
 * @details This `rt_gltf.c` fragment parses bounded JSON spans without constructing the runtime
 *          object tree, validates buffer-view and accessor ranges, converts primitive data into
 *          owned mesh and morph POD dependencies, and stages decoded or source image payloads.
 *          The resulting preload bundle transfers validated work to the synchronous commit path.
 */

/// @brief Resolve a raw JSON texture object's source, including KHR_texture_basisu.
/// @param[in] json Complete glTF JSON byte sequence.
/// @param len Length of @p json in bytes.
/// @param obj_start Inclusive byte offset of the texture object.
/// @param obj_end Exclusive byte offset bounding the texture object.
/// @return Basis Universal extension source when present and valid; otherwise the
///         texture's ordinary source index, or -1 when neither is declared.
static int gltf_json_texture_source_index(const char *json,
                                          size_t len,
                                          size_t obj_start,
                                          size_t obj_end) {
    size_t extensions_start;
    size_t extensions_end;
    size_t basisu_start;
    size_t basisu_end;
    int source = gltf_json_object_get_int(json, len, obj_start, obj_end, "source", -1);
    if (!gltf_json_object_find_value(
            json, len, obj_start, obj_end, "extensions", &extensions_start, &extensions_end))
        return source;
    extensions_start = gltf_json_skip_ws(json, len, extensions_start);
    if (extensions_start >= extensions_end || json[extensions_start] != '{')
        return source;
    if (!gltf_json_object_find_value(json,
                                     len,
                                     extensions_start,
                                     extensions_end,
                                     "KHR_texture_basisu",
                                     &basisu_start,
                                     &basisu_end))
        return source;
    basisu_start = gltf_json_skip_ws(json, len, basisu_start);
    if (basisu_start >= basisu_end || json[basisu_start] != '{')
        return source;
    {
        int basisu_source =
            gltf_json_object_get_int(json, len, basisu_start, basisu_end, "source", -1);
        return basisu_source >= 0 ? basisu_source : source;
    }
}

/// @brief Extract the glTF JSON text from raw bytes, handling both .gltf and binary .glb.
/// @details For GLB (magic "glTF", version 2) it validates the chunk table and copies the first
///          (JSON) chunk; for plain .gltf it copies the bytes verbatim. Returns a NUL-terminated
///          owned copy (caller frees) with @p out_len set, or NULL on a malformed container.
/// @param[in] data Root asset bytes containing JSON text or a GLB container.
/// @param len Number of readable bytes at @p data.
/// @param[out] out_len Optional destination for the copied JSON byte length, excluding NUL.
/// @return Malloc-owned, NUL-terminated JSON text, or `NULL` on invalid input or allocation
///         failure.
static char *gltf_json_copy_from_root_bytes(uint8_t *data, size_t len, size_t *out_len) {
    gltf_root_document_view document;
    char *json_copy;
    if (out_len)
        *out_len = 0;
    if (!gltf_parse_root_document_view(data, len, &document) || document.json_len > SIZE_MAX - 1u)
        return NULL;
    json_copy = (char *)malloc(document.json_len + 1u);
    if (!json_copy)
        return NULL;
    memcpy(json_copy, document.json, document.json_len);
    json_copy[document.json_len] = '\0';
    if (out_len)
        *out_len = document.json_len;
    return json_copy;
}

/// @brief Stage an image payload under @p key, decoding to RGBA up front when possible.
/// @details For a nonempty payload, transfers @p data to the bundle or releases it on a failure
///          before insertion. If it decodes to an RGBA blob the dependency is tagged IMAGE_RGBA
///          (ready for streaming upload); otherwise the raw bytes are kept as IMAGE. When
///          @p required and a supported format fails to decode, sets @p error and fails.
/// @param[in,out] bundle Bundle receiving the owned image dependencies.
/// @param[in] key Stable dependency key used for both source and decoded forms.
/// @param[in] mime_or_name MIME type or resource name used to select an image decoder.
/// @param[in,out] data Source payload whose ownership transfers when @p data_len is nonzero.
/// @param data_len Size of @p data in bytes.
/// @param required Non-zero when a supported but undecodable image must fail staging.
/// @param[out] error Optional caller-provided error buffer.
/// @param error_cap Capacity of @p error in bytes.
/// @return Non-zero on success, including the no-op for empty data; zero on a required
///         decode or dependency-staging failure.
static int gltf_preload_bundle_add_image_payload(rt_gltf_preload_bundle *bundle,
                                                 const char *key,
                                                 const char *mime_or_name,
                                                 uint8_t *data,
                                                 size_t data_len,
                                                 int required,
                                                 char *error,
                                                 size_t error_cap) {
    uint8_t *rgba_blob = NULL;
    size_t rgba_len = 0;
    gltf_preload_dependency_kind_t kind = GLTF_PRELOAD_DEP_IMAGE;
    if (!data || data_len == 0)
        return 1;
    if (gltf_decode_image_payload_to_rgba_blob(
            mime_or_name, data, data_len, &rgba_blob, &rgba_len)) {
        if (!gltf_preload_bundle_add_dependency(
                bundle, key, GLTF_PRELOAD_DEP_IMAGE_SOURCE, data, data_len)) {
            free(data);
            free(rgba_blob);
            gltf_preload_set_error(error, error_cap, "failed to stage glTF image source");
            return 0;
        }
        if (!gltf_preload_bundle_add_dependency(
                bundle, key, GLTF_PRELOAD_DEP_IMAGE_RGBA, rgba_blob, rgba_len)) {
            free(rgba_blob);
            gltf_preload_set_error(error, error_cap, "failed to stage glTF dependency");
            return 0;
        }
        return 1;
    }
    if (required && kind != GLTF_PRELOAD_DEP_IMAGE_RGBA &&
        gltf_preload_image_is_supported_format(mime_or_name)) {
        free(data);
        gltf_preload_set_error(error, error_cap, "invalid glTF image payload");
        return 0;
    }
    if (!gltf_preload_bundle_add_dependency(bundle, key, kind, data, data_len)) {
        free(data);
        gltf_preload_set_error(error, error_cap, "failed to stage glTF dependency");
        return 0;
    }
    return 1;
}

/// @brief Resolve and stage an external (non-data-URI) image referenced by @p uri.
/// @details Resolves @p uri relative to @p model_path, loads the bytes (from the asset system or
///          disk), and stages them via add_image_payload keyed by the resolved path. data: URIs and
///          unreadable optional images are skipped; an unreadable required image sets @p error.
/// @param[in,out] bundle Bundle receiving the staged image dependency.
/// @param[in] model_path Path of the root glTF asset used for relative resolution.
/// @param[in] uri External image URI from the glTF document.
/// @param load_assets Non-zero to resolve through the asset-loading system.
/// @param required Non-zero when a supported missing or invalid image is fatal.
/// @param[out] error Optional caller-provided error buffer.
/// @param error_cap Capacity of @p error in bytes.
/// @return Non-zero when the image was staged or may be skipped; zero on a required
///         image failure.
static int gltf_preload_stage_external_image(rt_gltf_preload_bundle *bundle,
                                             const char *model_path,
                                             const char *uri,
                                             int load_assets,
                                             int required,
                                             char *error,
                                             size_t error_cap) {
    char *resource_path;
    uint8_t *data;
    size_t data_len = 0;
    int ok;
    if (!uri || strncmp(uri, "data:", 5) == 0)
        return 1;
    resource_path = gltf_resolve_relative_path_alloc(model_path, uri);
    if (!resource_path)
        return 1;
    data = gltf_load_dependency_bytes(resource_path, load_assets, 0u, NULL, &data_len);
    if (!data) {
        if (required && gltf_preload_image_is_supported_format(resource_path)) {
            gltf_preload_set_error(error, error_cap, "failed to stage glTF dependency");
            free(resource_path);
            return 0;
        }
        free(resource_path);
        return 1;
    }
    ok = gltf_preload_bundle_add_image_payload(
        bundle, resource_path, resource_path, data, data_len, required, error, error_cap);
    free(resource_path);
    return ok;
}

/// @brief Format the dependency key for inline buffer @p index ("<gltf:inline-buffer:N>").
/// @param index Zero-based glTF buffer index.
/// @param[out] out Destination character buffer.
/// @param out_cap Capacity of @p out, including the terminator.
static void gltf_preload_buffer_key(int index, char *out, size_t out_cap) {
    if (!out || out_cap == 0)
        return;
    snprintf(out, out_cap, "<gltf:inline-buffer:%d>", index);
}

/// @brief Format the dependency key for inline image @p index, with an extension chosen by MIME
/// type.
/// @param index Zero-based glTF image index.
/// @param[in] mime_type Optional MIME type used to choose a recognizable suffix.
/// @param[out] out Destination character buffer.
/// @param out_cap Capacity of @p out, including the terminator.
static void gltf_preload_image_key(int index, const char *mime_type, char *out, size_t out_cap) {
    const char *ext = ".bin";
    if (!out || out_cap == 0)
        return;
    if (mime_type) {
        if (strstr(mime_type, "png"))
            ext = ".png";
        else if (strstr(mime_type, "jpeg") || strstr(mime_type, "jpg"))
            ext = ".jpg";
        else if (strstr(mime_type, "bmp"))
            ext = ".bmp";
        else if (strstr(mime_type, "gif"))
            ext = ".gif";
    }
    snprintf(out, out_cap, "inline-image-%d%s", index, ext);
}

/// @brief Format the mesh-POD dependency key for a (mesh, primitive) pair.
/// @param mesh_index Zero-based source mesh index.
/// @param primitive_index Zero-based primitive index within the source mesh.
/// @param[out] out Destination character buffer.
/// @param out_cap Capacity of @p out, including the terminator.
static void gltf_preload_mesh_key(int mesh_index, int primitive_index, char *out, size_t out_cap) {
    if (!out || out_cap == 0)
        return;
    snprintf(out, out_cap, "<gltf:mesh-pod:%d:%d>", mesh_index, primitive_index);
}

/// @brief Format the morph-POD dependency key for a (mesh, primitive) pair.
/// @param mesh_index Zero-based source mesh index.
/// @param primitive_index Zero-based primitive index within the source mesh.
/// @param[out] out Destination character buffer.
/// @param out_cap Capacity of @p out, including the terminator.
static void gltf_preload_morph_key(int mesh_index, int primitive_index, char *out, size_t out_cap) {
    if (!out || out_cap == 0)
        return;
    snprintf(out, out_cap, "<gltf:morph-pod:%d:%d>", mesh_index, primitive_index);
}

/// @brief Locate the binary (BIN) chunk inside a GLB container.
/// @details Validates the "glTF" magic/version/length and walks the chunk table (the JSON chunk
///          must come first) to find the 0x004E4942 ("BIN\0") chunk.
/// @param[in] data Root asset bytes.
/// @param len Number of readable bytes at @p data.
/// @param[out] out_bin Optional destination for the borrowed BIN payload pointer.
/// @param[out] out_bin_len Optional destination for the BIN payload length.
/// @return Non-zero with the requested outputs set for a valid GLB BIN chunk; zero for
///         plain JSON, malformed GLB data, or an absent BIN chunk.
static int gltf_root_find_glb_bin(uint8_t *data,
                                  size_t len,
                                  const uint8_t **out_bin,
                                  size_t *out_bin_len) {
    gltf_root_document_view document;
    if (out_bin)
        *out_bin = NULL;
    if (out_bin_len)
        *out_bin_len = 0;
    if (!gltf_parse_root_document_view(data, len, &document) || !document.is_glb || !document.bin)
        return 0;
    if (out_bin)
        *out_bin = document.bin;
    if (out_bin_len)
        *out_bin_len = document.bin_len;
    return 1;
}

/// @brief Ensure the buffer-ref array holds at least @p required_count entries, zero-filling
/// growth.
/// @param[in,out] refs Address of the reallocatable buffer-reference array.
/// @param[in,out] capacity Current capacity, updated after successful growth.
/// @param required_count Minimum number of entries required.
/// @return Non-zero when the requested capacity is available; zero on invalid arguments,
///         overflow, or allocation failure.
static int gltf_preload_grow_buffer_refs(gltf_preload_buffer_ref_t **refs,
                                         int *capacity,
                                         int required_count) {
    gltf_preload_buffer_ref_t *grown;
    int next_capacity;
    if (!refs || !capacity || required_count <= 0)
        return 0;
    if (*capacity >= required_count)
        return 1;
    if (!gltf_preload_next_capacity_int(
            *capacity, required_count, 8, sizeof(**refs), &next_capacity))
        return 0;
    grown = (gltf_preload_buffer_ref_t *)realloc(*refs, (size_t)next_capacity * sizeof(**refs));
    if (!grown)
        return 0;
    memset(grown + *capacity, 0, (size_t)(next_capacity - *capacity) * sizeof(*grown));
    *refs = grown;
    *capacity = next_capacity;
    return 1;
}

/// @brief Bind one staged buffer reference to a borrowed byte range.
/// @param[out] refs Buffer-reference table containing @p index.
/// @param index Zero-based table entry to update.
/// @param[in] data Borrowed buffer payload retained by the preload bundle or root asset.
/// @param len Number of readable bytes at @p data.
static void gltf_preload_set_buffer_ref(gltf_preload_buffer_ref_t *refs,
                                        int index,
                                        const uint8_t *data,
                                        size_t len) {
    refs[index].data = data;
    refs[index].len = len;
}

/// @brief Decode and stage one data-URI buffer dependency.
/// @param[in,out] bundle Bundle that assumes ownership of successfully decoded bytes.
/// @param index Zero-based glTF buffer index.
/// @param[in] uri Data URI containing the buffer payload.
/// @param required_len Minimum payload length declared by the buffer object.
/// @param[out] refs Buffer-reference table receiving the staged payload.
/// @return Non-zero when the payload satisfies the declared length and is staged, or
///         when an empty declared buffer may be omitted; zero otherwise.
static int gltf_preload_stage_inline_buffer(rt_gltf_preload_bundle *bundle,
                                            int index,
                                            const char *uri,
                                            size_t required_len,
                                            gltf_preload_buffer_ref_t *refs) {
    char key[64];
    char mime_type[64];
    uint8_t *data = NULL;
    size_t data_len = 0;
    if (!gltf_parse_data_uri(uri, mime_type, sizeof(mime_type), &data, &data_len) ||
        data_len < required_len) {
        free(data);
        return required_len == 0u;
    }
    gltf_preload_buffer_key(index, key, sizeof(key));
    if (!gltf_preload_bundle_add_dependency(bundle, key, GLTF_PRELOAD_DEP_BUFFER, data, data_len)) {
        free(data);
        return 0;
    }
    gltf_preload_set_buffer_ref(refs, index, data, data_len);
    return 1;
}

/// @brief Resolve, load, and stage one external buffer dependency.
/// @param[in,out] bundle Bundle that assumes ownership of successfully loaded bytes.
/// @param[in] model_path Root asset path used to resolve the relative URI.
/// @param index Zero-based glTF buffer index.
/// @param[in] uri External buffer URI.
/// @param load_assets Non-zero to resolve through the asset-loading system.
/// @param required_len Minimum number of bytes required by the buffer declaration.
/// @param[out] refs Buffer-reference table receiving the staged payload.
/// @return Non-zero when the dependency was staged or the declaration permits an empty
///         payload; zero when a required payload cannot be loaded or recorded.
static int gltf_preload_stage_external_buffer(rt_gltf_preload_bundle *bundle,
                                              const char *model_path,
                                              int index,
                                              const char *uri,
                                              int load_assets,
                                              size_t required_len,
                                              gltf_preload_buffer_ref_t *refs) {
    char *resource_path;
    uint8_t *data = NULL;
    size_t data_len = 0;
    resource_path = gltf_resolve_relative_path_alloc(model_path, uri);
    if (resource_path) {
        data =
            gltf_load_dependency_bytes(resource_path, load_assets, required_len, NULL, &data_len);
        if (data) {
            if (!gltf_preload_bundle_add_dependency(
                    bundle, resource_path, GLTF_PRELOAD_DEP_BUFFER, data, data_len)) {
                free(data);
                free(resource_path);
                return 0;
            }
            gltf_preload_set_buffer_ref(refs, index, data, data_len);
        }
        free(resource_path);
    }
    return data || required_len == 0u;
}

/// @brief Dispatch a buffer URI to inline or external dependency staging.
/// @param[in,out] bundle Bundle receiving the staged buffer.
/// @param[in] model_path Root asset path used for relative external URIs.
/// @param index Zero-based glTF buffer index.
/// @param[in] uri Data or external URI to stage.
/// @param load_assets Non-zero to use asset-system loading for external data.
/// @param required_len Minimum payload length.
/// @param[out] refs Buffer-reference table receiving the payload.
/// @return Result of the selected buffer staging path.
static int gltf_preload_stage_buffer_uri(rt_gltf_preload_bundle *bundle,
                                         const char *model_path,
                                         int index,
                                         const char *uri,
                                         int load_assets,
                                         size_t required_len,
                                         gltf_preload_buffer_ref_t *refs) {
    if (strncmp(uri, "data:", 5) == 0)
        return gltf_preload_stage_inline_buffer(bundle, index, uri, required_len, refs);
    return gltf_preload_stage_external_buffer(
        bundle, model_path, index, uri, load_assets, required_len, refs);
}

/// @brief Bind the URI-less first glTF buffer to a validated GLB BIN chunk.
/// @param index Zero-based glTF buffer index; only index zero is accepted.
/// @param[in] glb_bin Borrowed GLB BIN payload.
/// @param glb_bin_len Physical BIN chunk length, including permitted alignment padding.
/// @param required_len Logical byte length declared by the glTF buffer.
/// @param[out] refs Buffer-reference table receiving the logical BIN range.
/// @return Non-zero when the BIN chunk matches the declaration within GLB padding rules.
static int gltf_preload_bind_glb_buffer(int index,
                                        const uint8_t *glb_bin,
                                        size_t glb_bin_len,
                                        size_t required_len,
                                        gltf_preload_buffer_ref_t *refs) {
    if (index != 0 || !glb_bin || required_len > glb_bin_len || required_len > SIZE_MAX - 3u ||
        glb_bin_len > required_len + 3u)
        return 0;
    gltf_preload_set_buffer_ref(refs, index, glb_bin, required_len);
    return 1;
}

/// @brief Stage every glTF buffer and build the buffer-ref table the views/accessors resolve
/// against.
/// @details Walks the top-level "buffers" array: a data: URI is decoded inline, an external URI is
///          resolved/loaded, and an absent URI binds to the GLB BIN chunk. Each ref records the
///          bytes so later stages can slice them. Sets @p error and returns 0 on a staging failure.
/// @param[in,out] bundle Bundle receiving owned buffer dependencies.
/// @param[in] model_path Root asset path used for relative URI resolution.
/// @param[in] json Complete glTF JSON text.
/// @param json_len Length of @p json in bytes.
/// @param load_assets Non-zero to use asset-system loading for external buffers.
/// @param[out] out_refs Optional destination for the malloc-owned buffer-reference table.
/// @param[out] out_count Optional destination for the number of parsed buffer entries.
/// @param[out] error Optional caller-provided error buffer.
/// @param error_cap Capacity of @p error in bytes.
/// @return Non-zero when all declared buffers are staged or no buffer array exists;
///         zero on malformed sizing, allocation, or dependency-loading failure.
static int gltf_preload_stage_buffers(rt_gltf_preload_bundle *bundle,
                                      const char *model_path,
                                      const char *json,
                                      size_t json_len,
                                      int load_assets,
                                      gltf_preload_buffer_ref_t **out_refs,
                                      int *out_count,
                                      char *error,
                                      size_t error_cap) {
    size_t array_start;
    size_t array_end;
    size_t pos;
    const uint8_t *glb_bin = NULL;
    size_t glb_bin_len = 0;
    gltf_preload_buffer_ref_t *refs = NULL;
    int refs_capacity = 0;
    int index = 0;
    if (out_refs)
        *out_refs = NULL;
    if (out_count)
        *out_count = 0;
    gltf_root_find_glb_bin(
        bundle ? bundle->root_data : NULL, bundle ? bundle->root_size : 0, &glb_bin, &glb_bin_len);
    if (!gltf_json_find_top_level_array(json, json_len, "buffers", &array_start, &array_end))
        return 1;
    pos = array_start + 1u;
    while (pos < array_end) {
        pos = gltf_json_skip_ws(json, json_len, pos);
        if (pos >= array_end || json[pos] == ']')
            break;
        if (json[pos] == '{') {
            size_t object_end = gltf_json_find_matching(json, json_len, pos, '{', '}');
            char *uri;
            size_t required_len = 0;
            if (object_end == SIZE_MAX || object_end > array_end)
                break;
            if (!rt_untrusted_count_ok((int64_t)index + 1, 1u, json_len) ||
                !gltf_preload_grow_buffer_refs(&refs, &refs_capacity, index + 1)) {
                free(refs);
                gltf_preload_set_error(error, error_cap, "failed to stage glTF dependency");
                return 0;
            }
            uri = gltf_json_object_get_string(json, json_len, pos, object_end, "uri");
            required_len =
                gltf_json_object_get_size(json, json_len, pos, object_end, "byteLength", 0u);
            if (uri) {
                int ok = gltf_preload_stage_buffer_uri(
                    bundle, model_path, index, uri, load_assets, required_len, refs);
                free(uri);
                if (!ok) {
                    free(refs);
                    gltf_preload_set_error(error, error_cap, "failed to stage glTF dependency");
                    return 0;
                }
            } else {
                if (!gltf_preload_bind_glb_buffer(
                        index, glb_bin, glb_bin_len, required_len, refs)) {
                    free(refs);
                    gltf_preload_set_error(error, error_cap, "failed to stage glTF dependency");
                    return 0;
                }
            }
            index++;
            pos = object_end;
        } else {
            pos = gltf_json_skip_value(json, json_len, pos);
            if (pos == SIZE_MAX)
                break;
        }
        pos = gltf_json_skip_ws(json, json_len, pos);
        if (pos < array_end && json[pos] == ',')
            pos++;
    }
    if (out_refs)
        *out_refs = refs;
    else
        free(refs);
    if (out_count)
        *out_count = index;
    return 1;
}

/// @brief Ensure the buffer-view-ref array holds at least @p required_count entries (zero-filled
/// growth).
/// @param[in,out] views Address of the reallocatable buffer-view-reference array.
/// @param[in,out] capacity Current capacity, updated after successful growth.
/// @param required_count Minimum number of entries required.
/// @return Non-zero when the requested capacity is available; zero on invalid arguments,
///         overflow, or allocation failure.
static int gltf_preload_grow_view_refs(gltf_preload_buffer_view_ref_t **views,
                                       int *capacity,
                                       int required_count) {
    gltf_preload_buffer_view_ref_t *grown;
    int next_capacity;
    if (!views || !capacity || required_count <= 0)
        return 0;
    if (*capacity >= required_count)
        return 1;
    if (!gltf_preload_next_capacity_int(
            *capacity, required_count, 8, sizeof(**views), &next_capacity))
        return 0;
    grown =
        (gltf_preload_buffer_view_ref_t *)realloc(*views, (size_t)next_capacity * sizeof(**views));
    if (!grown)
        return 0;
    memset(grown + *capacity, 0, (size_t)(next_capacity - *capacity) * sizeof(*grown));
    *views = grown;
    *capacity = next_capacity;
    return 1;
}

/// @brief Parse the top-level "bufferViews" array into a ref table (buffer, offset, length,
/// stride).
/// @details A view is marked valid when it names a buffer and has a positive length. Returns the
///          table via @p out_views / @p out_count; on allocation failure sets @p error and returns
///          0.
/// @param[in] json Complete glTF JSON text.
/// @param json_len Length of @p json in bytes.
/// @param[out] out_views Optional destination for the malloc-owned view-reference table.
/// @param[out] out_count Optional destination for the number of parsed buffer views.
/// @param[out] error Optional caller-provided error buffer.
/// @param error_cap Capacity of @p error in bytes.
/// @return Non-zero on successful parsing or when no buffer-view array exists; zero on
///         count validation or allocation failure.
static int gltf_preload_parse_buffer_views(const char *json,
                                           size_t json_len,
                                           gltf_preload_buffer_view_ref_t **out_views,
                                           int *out_count,
                                           char *error,
                                           size_t error_cap) {
    size_t array_start;
    size_t array_end;
    size_t pos;
    gltf_preload_buffer_view_ref_t *views = NULL;
    int views_capacity = 0;
    int index = 0;
    if (out_views)
        *out_views = NULL;
    if (out_count)
        *out_count = 0;
    if (!gltf_json_find_top_level_array(json, json_len, "bufferViews", &array_start, &array_end))
        return 1;
    pos = array_start + 1u;
    while (pos < array_end) {
        pos = gltf_json_skip_ws(json, json_len, pos);
        if (pos >= array_end || json[pos] == ']')
            break;
        if (json[pos] == '{') {
            size_t object_end = gltf_json_find_matching(json, json_len, pos, '{', '}');
            if (object_end == SIZE_MAX || object_end > array_end)
                break;
            if (!rt_untrusted_count_ok((int64_t)index + 1, 1u, json_len) ||
                !gltf_preload_grow_view_refs(&views, &views_capacity, index + 1)) {
                free(views);
                gltf_preload_set_error(error, error_cap, "failed to stage glTF dependency");
                return 0;
            }
            views[index].buffer =
                gltf_json_object_get_int(json, json_len, pos, object_end, "buffer", -1);
            views[index].byte_offset =
                gltf_json_object_get_size(json, json_len, pos, object_end, "byteOffset", 0u);
            views[index].byte_length =
                gltf_json_object_get_size(json, json_len, pos, object_end, "byteLength", 0u);
            views[index].byte_stride =
                gltf_json_object_get_size(json, json_len, pos, object_end, "byteStride", 0u);
            views[index].valid = views[index].buffer >= 0 && views[index].byte_length > 0;
            /* EXT_meshopt_compression views resolve through the sync loader's decoded
             * shadow table; the parent range may point at a data-less placeholder
             * buffer, so worker staging must not read it. Marking the view invalid
             * makes dependent primitives fall back to the commit-time sync decode. */
            {
                size_t ext_start;
                size_t ext_end;
                if (gltf_json_object_find_value(
                        json, json_len, pos, object_end, "extensions", &ext_start, &ext_end) &&
                    ext_start < ext_end && json[ext_start] == '{' &&
                    gltf_json_object_find_value(json,
                                                json_len,
                                                ext_start,
                                                ext_end,
                                                "EXT_meshopt_compression",
                                                &ext_start,
                                                &ext_end))
                    views[index].valid = 0;
            }
            index++;
            pos = object_end;
        } else {
            pos = gltf_json_skip_value(json, json_len, pos);
            if (pos == SIZE_MAX)
                break;
        }
        pos = gltf_json_skip_ws(json, json_len, pos);
        if (pos < array_end && json[pos] == ',')
            pos++;
    }
    if (out_views)
        *out_views = views;
    else
        free(views);
    if (out_count)
        *out_count = index;
    return 1;
}

/// @brief Verify one accessor's element range lies entirely within its buffer view and buffer.
/// @details Computes element size from componentType x type, derives the stride (explicit or
///          tightly-packed), and overflow-checks that the last element's end offset fits both the
///          view's byteLength and the backing buffer. This is the security gate that stops a
///          malformed accessor from inducing out-of-bounds reads during mesh packing.
/// @param[in] json Complete glTF JSON text.
/// @param json_len Length of @p json in bytes.
/// @param obj_start Inclusive byte offset of the accessor object.
/// @param obj_end Exclusive byte offset bounding the accessor object.
/// @param[in] buffers Staged buffer-reference table.
/// @param buffer_count Number of entries in @p buffers.
/// @param[in] views Parsed buffer-view-reference table.
/// @param view_count Number of entries in @p views.
/// @return Non-zero if valid, including a zero count or absent buffer view; zero if
///         component metadata is invalid or any computed range exceeds its bounds.
static int gltf_preload_validate_accessor(const char *json,
                                          size_t json_len,
                                          size_t obj_start,
                                          size_t obj_end,
                                          const gltf_preload_buffer_ref_t *buffers,
                                          int buffer_count,
                                          const gltf_preload_buffer_view_ref_t *views,
                                          int view_count) {
    int view_index;
    int component_type;
    int component_size;
    int component_count;
    int count;
    size_t accessor_offset;
    size_t element_size;
    size_t stride;
    size_t last_offset;
    size_t accessor_last;
    size_t accessor_end;
    size_t buffer_end;
    char *type;
    const gltf_preload_buffer_view_ref_t *view;

    count = gltf_json_object_get_int(json, json_len, obj_start, obj_end, "count", -1);
    if (count < 0)
        return 0;
    if (count == 0)
        return 1;

    component_type =
        gltf_json_object_get_int(json, json_len, obj_start, obj_end, "componentType", 0);
    component_size = gltf_component_size(component_type);
    type = gltf_json_object_get_string(json, json_len, obj_start, obj_end, "type");
    component_count = gltf_component_count(type);
    free(type);
    if (component_size <= 0 || component_count <= 0)
        return 0;
    if (!gltf_checked_mul_size((size_t)component_size, (size_t)component_count, &element_size))
        return 0;

    view_index = gltf_json_object_get_int(json, json_len, obj_start, obj_end, "bufferView", -1);
    if (view_index < 0)
        return 1;
    if (!views || view_index >= view_count || !views[view_index].valid)
        return 0;
    view = &views[view_index];
    if (view->buffer < 0 || view->buffer >= buffer_count || !buffers || !buffers[view->buffer].data)
        return 0;

    accessor_offset =
        gltf_json_object_get_size(json, json_len, obj_start, obj_end, "byteOffset", 0u);
    stride = view->byte_stride > 0u ? view->byte_stride : element_size;
    if (stride < element_size)
        return 0;
    if (accessor_offset > view->byte_length ||
        !rt_untrusted_count_ok((int64_t)count, element_size, view->byte_length - accessor_offset))
        return 0;
    if (!gltf_checked_mul_size((size_t)(count - 1), stride, &last_offset) ||
        !gltf_checked_add_size(accessor_offset, last_offset, &accessor_last) ||
        !gltf_checked_add_size(accessor_last, element_size, &accessor_end))
        return 0;
    if (accessor_end > view->byte_length)
        return 0;
    if (!gltf_checked_add_size(view->byte_offset, accessor_end, &buffer_end))
        return 0;
    return buffer_end <= buffers[view->buffer].len;
}

/// @brief Validate every accessor in the top-level "accessors" array against its view/buffer
/// bounds.
/// @details Fails fast (setting @p error) on the first out-of-range accessor. Returns 1 if all
/// pass.
/// @param[in] json Complete glTF JSON text.
/// @param json_len Length of @p json in bytes.
/// @param[in] buffers Staged buffer-reference table.
/// @param buffer_count Number of entries in @p buffers.
/// @param[in] views Parsed buffer-view-reference table.
/// @param view_count Number of entries in @p views.
/// @param[out] error Optional caller-provided error buffer.
/// @param error_cap Capacity of @p error in bytes.
/// @return Non-zero when all accessor ranges are valid or no accessor array exists;
///         zero after reporting the first invalid range.
static int gltf_preload_validate_accessors(const char *json,
                                           size_t json_len,
                                           const gltf_preload_buffer_ref_t *buffers,
                                           int buffer_count,
                                           const gltf_preload_buffer_view_ref_t *views,
                                           int view_count,
                                           char *error,
                                           size_t error_cap) {
    size_t array_start;
    size_t array_end;
    size_t pos;
    if (!gltf_json_find_top_level_array(json, json_len, "accessors", &array_start, &array_end))
        return 1;
    pos = array_start + 1u;
    while (pos < array_end) {
        pos = gltf_json_skip_ws(json, json_len, pos);
        if (pos >= array_end || json[pos] == ']')
            break;
        if (json[pos] == '{') {
            size_t object_end = gltf_json_find_matching(json, json_len, pos, '{', '}');
            if (object_end == SIZE_MAX || object_end > array_end)
                break;
            if (!gltf_preload_validate_accessor(
                    json, json_len, pos, object_end, buffers, buffer_count, views, view_count)) {
                gltf_preload_set_error(error, error_cap, "invalid glTF accessor range");
                return 0;
            }
            pos = object_end;
        } else {
            pos = gltf_json_skip_value(json, json_len, pos);
            if (pos == SIZE_MAX)
                break;
        }
        pos = gltf_json_skip_ws(json, json_len, pos);
        if (pos < array_end && json[pos] == ',')
            pos++;
    }
    return 1;
}

/// @brief Ensure the accessor-ref array holds at least @p required_count entries (zero-filled
/// growth).
/// @param[in,out] accessors Address of the reallocatable accessor-reference array.
/// @param[in,out] capacity Current capacity, updated after successful growth.
/// @param required_count Minimum number of entries required.
/// @return Non-zero when the requested capacity is available; zero on invalid arguments,
///         overflow, or allocation failure.
static int gltf_preload_grow_accessor_refs(gltf_preload_accessor_ref_t **accessors,
                                           int *capacity,
                                           int required_count) {
    gltf_preload_accessor_ref_t *grown;
    int next_capacity;
    if (!accessors || !capacity || required_count <= 0)
        return 0;
    if (*capacity >= required_count)
        return 1;
    if (!gltf_preload_next_capacity_int(
            *capacity, required_count, 8, sizeof(**accessors), &next_capacity))
        return 0;
    grown = (gltf_preload_accessor_ref_t *)realloc(*accessors,
                                                   (size_t)next_capacity * sizeof(**accessors));
    if (!grown)
        return 0;
    memset(grown + *capacity, 0, (size_t)(next_capacity - *capacity) * sizeof(*grown));
    *accessors = grown;
    *capacity = next_capacity;
    return 1;
}

/// @brief Parse the top-level "accessors" array (including sparse substitutions) into a ref table.
/// @details Records component type/count, element count, normalization, and any sparse
///          indices/values sub-views. Marks each accessor valid only with a positive count and
///          known component size. Returns the table via @p out_accessors / @p out_count.
/// @param[in] json Complete glTF JSON text.
/// @param json_len Length of @p json in bytes.
/// @param[out] out_accessors Optional destination for the malloc-owned accessor table.
/// @param[out] out_count Optional destination for the number of parsed accessors.
/// @param[out] error Optional caller-provided error buffer.
/// @param error_cap Capacity of @p error in bytes.
/// @return Non-zero on successful parsing or when no accessor array exists; zero on
///         count validation or allocation failure.
static int gltf_preload_parse_accessors(const char *json,
                                        size_t json_len,
                                        gltf_preload_accessor_ref_t **out_accessors,
                                        int *out_count,
                                        char *error,
                                        size_t error_cap) {
    size_t array_start;
    size_t array_end;
    size_t pos;
    gltf_preload_accessor_ref_t *accessors = NULL;
    int accessors_capacity = 0;
    int index = 0;
    if (out_accessors)
        *out_accessors = NULL;
    if (out_count)
        *out_count = 0;
    if (!gltf_json_find_top_level_array(json, json_len, "accessors", &array_start, &array_end))
        return 1;
    pos = array_start + 1u;
    while (pos < array_end) {
        pos = gltf_json_skip_ws(json, json_len, pos);
        if (pos >= array_end || json[pos] == ']')
            break;
        if (json[pos] == '{') {
            size_t object_end = gltf_json_find_matching(json, json_len, pos, '{', '}');
            char *type;
            size_t sparse_start;
            size_t sparse_end;
            if (object_end == SIZE_MAX || object_end > array_end)
                break;
            if (!rt_untrusted_count_ok((int64_t)index + 1, 1u, json_len) ||
                !gltf_preload_grow_accessor_refs(&accessors, &accessors_capacity, index + 1)) {
                free(accessors);
                gltf_preload_set_error(error, error_cap, "failed to stage glTF dependency");
                return 0;
            }
            type = gltf_json_object_get_string(json, json_len, pos, object_end, "type");
            accessors[index].view =
                gltf_json_object_get_int(json, json_len, pos, object_end, "bufferView", -1);
            accessors[index].sparse_indices_view = -1;
            accessors[index].sparse_values_view = -1;
            accessors[index].byte_offset =
                gltf_json_object_get_size(json, json_len, pos, object_end, "byteOffset", 0u);
            accessors[index].comp_type =
                gltf_json_object_get_int(json, json_len, pos, object_end, "componentType", 0);
            accessors[index].comp_count = gltf_component_count(type);
            accessors[index].count =
                gltf_json_object_get_int(json, json_len, pos, object_end, "count", 0);
            accessors[index].normalized = (int8_t)gltf_json_object_get_boolish(
                json, json_len, pos, object_end, "normalized", 0);
            if (gltf_json_object_find_value(
                    json, json_len, pos, object_end, "sparse", &sparse_start, &sparse_end) &&
                sparse_start < sparse_end && json[sparse_start] == '{') {
                size_t indices_start;
                size_t indices_end;
                size_t values_start;
                size_t values_end;
                accessors[index].sparse_count =
                    gltf_json_object_get_int(json, json_len, sparse_start, sparse_end, "count", 0);
                if (accessors[index].sparse_count > 0 &&
                    gltf_json_object_find_value(json,
                                                json_len,
                                                sparse_start,
                                                sparse_end,
                                                "indices",
                                                &indices_start,
                                                &indices_end) &&
                    gltf_json_object_find_value(json,
                                                json_len,
                                                sparse_start,
                                                sparse_end,
                                                "values",
                                                &values_start,
                                                &values_end) &&
                    indices_start < indices_end && values_start < values_end &&
                    json[indices_start] == '{' && json[values_start] == '{') {
                    accessors[index].has_sparse = 1;
                    accessors[index].sparse_indices_view = gltf_json_object_get_int(
                        json, json_len, indices_start, indices_end, "bufferView", -1);
                    accessors[index].sparse_indices_offset = gltf_json_object_get_size(
                        json, json_len, indices_start, indices_end, "byteOffset", 0u);
                    accessors[index].sparse_index_comp_type = gltf_json_object_get_int(
                        json, json_len, indices_start, indices_end, "componentType", 0);
                    accessors[index].sparse_values_view = gltf_json_object_get_int(
                        json, json_len, values_start, values_end, "bufferView", -1);
                    accessors[index].sparse_values_offset = gltf_json_object_get_size(
                        json, json_len, values_start, values_end, "byteOffset", 0u);
                }
            }
            accessors[index].valid = accessors[index].count > 0 &&
                                     gltf_component_size(accessors[index].comp_type) > 0 &&
                                     accessors[index].comp_count > 0;
            free(type);
            index++;
            pos = object_end;
        } else {
            pos = gltf_json_skip_value(json, json_len, pos);
            if (pos == SIZE_MAX)
                break;
        }
        pos = gltf_json_skip_ws(json, json_len, pos);
        if (pos < array_end && json[pos] == ',')
            pos++;
    }
    if (out_accessors)
        *out_accessors = accessors;
    else
        free(accessors);
    if (out_count)
        *out_count = index;
    return 1;
}

/// @brief Resolve an accessor index to a concrete, bounds-checked view over buffer memory.
/// @details Computes element size and stride, validates the dense range fits the view and buffer,
///          and — for sparse accessors — validates the index/value sub-views and that sparse
///          indices are strictly increasing and in-range. Fills @p out with raw pointers the
///          packers read.
/// @param[in] accessors Parsed accessor-reference table.
/// @param accessor_count Number of entries in @p accessors.
/// @param accessor_index Zero-based accessor to resolve.
/// @param[in] buffers Staged buffer-reference table.
/// @param buffer_count Number of entries in @p buffers.
/// @param[in] views Parsed buffer-view-reference table.
/// @param view_count Number of entries in @p views.
/// @param[out] out Destination concrete accessor view.
/// @return Non-zero with @p out populated, or zero if the accessor is invalid or any
///         dense/sparse range or ordering check fails.
static int gltf_preload_resolve_accessor_view(const gltf_preload_accessor_ref_t *accessors,
                                              int accessor_count,
                                              int accessor_index,
                                              const gltf_preload_buffer_ref_t *buffers,
                                              int buffer_count,
                                              const gltf_preload_buffer_view_ref_t *views,
                                              int view_count,
                                              gltf_accessor_view_t *out) {
    const gltf_preload_accessor_ref_t *accessor;
    const gltf_preload_buffer_view_ref_t *view;
    size_t element_size;
    size_t stride;
    size_t last_offset;
    size_t accessor_last;
    size_t accessor_end;
    size_t buffer_offset;
    size_t buffer_end;
    int comp_size;
    if (!accessors || accessor_index < 0 || accessor_index >= accessor_count || !out)
        return 0;
    accessor = &accessors[accessor_index];
    if (!accessor->valid)
        return 0;
    comp_size = gltf_component_size(accessor->comp_type);
    if (comp_size <= 0 || accessor->comp_count <= 0 || accessor->count <= 0)
        return 0;
    if (!gltf_checked_mul_size((size_t)comp_size, (size_t)accessor->comp_count, &element_size))
        return 0;
    if (element_size > INT32_MAX)
        return 0;
    memset(out, 0, sizeof(*out));
    out->count = (int32_t)accessor->count;
    out->stride = (int32_t)element_size;
    out->comp_type = accessor->comp_type;
    out->comp_count = accessor->comp_count;
    out->normalized = accessor->normalized;
    if (accessor->view >= 0) {
        if (accessor->view >= view_count)
            return 0;
        view = views ? &views[accessor->view] : NULL;
        if (!view || !view->valid || view->buffer < 0 || view->buffer >= buffer_count || !buffers ||
            !buffers[view->buffer].data)
            return 0;
        stride = view->byte_stride > 0u ? view->byte_stride : element_size;
        if (stride < element_size || stride > INT32_MAX)
            return 0;
        if (accessor->byte_offset > view->byte_length ||
            !rt_untrusted_count_ok(
                (int64_t)accessor->count, element_size, view->byte_length - accessor->byte_offset))
            return 0;
        if (!gltf_checked_mul_size((size_t)(accessor->count - 1), stride, &last_offset) ||
            !gltf_checked_add_size(accessor->byte_offset, last_offset, &accessor_last) ||
            !gltf_checked_add_size(accessor_last, element_size, &accessor_end))
            return 0;
        if (accessor_end > view->byte_length)
            return 0;
        if (!gltf_checked_add_size(view->byte_offset, accessor->byte_offset, &buffer_offset) ||
            !gltf_checked_add_size(view->byte_offset, accessor_end, &buffer_end))
            return 0;
        if (buffer_end > buffers[view->buffer].len)
            return 0;
        out->data = buffers[view->buffer].data + buffer_offset;
        out->stride = (int32_t)stride;
    } else if (!accessor->has_sparse) {
        return 0;
    }
    if (accessor->has_sparse) {
        const gltf_preload_buffer_view_ref_t *indices_view;
        const gltf_preload_buffer_view_ref_t *values_view;
        int index_comp_size = gltf_component_size(accessor->sparse_index_comp_type);
        size_t index_bytes;
        size_t value_bytes;
        size_t index_end;
        size_t value_end;
        size_t index_buffer_offset;
        size_t value_buffer_offset;
        uint32_t previous_sparse_index = 0;
        if (accessor->sparse_count <= 0 ||
            (accessor->sparse_index_comp_type != 5121 && accessor->sparse_index_comp_type != 5123 &&
             accessor->sparse_index_comp_type != 5125) ||
            index_comp_size <= 0 || accessor->sparse_indices_view < 0 ||
            accessor->sparse_indices_view >= view_count || accessor->sparse_values_view < 0 ||
            accessor->sparse_values_view >= view_count || !views || !buffers)
            return 0;
        indices_view = &views[accessor->sparse_indices_view];
        values_view = &views[accessor->sparse_values_view];
        if (!indices_view->valid || !values_view->valid || indices_view->buffer < 0 ||
            values_view->buffer < 0 || indices_view->buffer >= buffer_count ||
            values_view->buffer >= buffer_count || !buffers[indices_view->buffer].data ||
            !buffers[values_view->buffer].data)
            return 0;
        if (accessor->sparse_indices_offset > indices_view->byte_length ||
            accessor->sparse_values_offset > values_view->byte_length ||
            !rt_untrusted_count_ok((int64_t)accessor->sparse_count,
                                   (size_t)index_comp_size,
                                   indices_view->byte_length - accessor->sparse_indices_offset) ||
            !rt_untrusted_count_ok((int64_t)accessor->sparse_count,
                                   element_size,
                                   values_view->byte_length - accessor->sparse_values_offset))
            return 0;
        if (!gltf_checked_mul_size(
                (size_t)accessor->sparse_count, (size_t)index_comp_size, &index_bytes) ||
            !gltf_checked_mul_size((size_t)accessor->sparse_count, element_size, &value_bytes) ||
            !gltf_checked_add_size(accessor->sparse_indices_offset, index_bytes, &index_end) ||
            !gltf_checked_add_size(accessor->sparse_values_offset, value_bytes, &value_end))
            return 0;
        if (index_end > indices_view->byte_length || value_end > values_view->byte_length)
            return 0;
        if (!gltf_checked_add_size(
                indices_view->byte_offset, accessor->sparse_indices_offset, &index_buffer_offset) ||
            !gltf_checked_add_size(
                values_view->byte_offset, accessor->sparse_values_offset, &value_buffer_offset) ||
            !gltf_checked_add_size(index_buffer_offset, index_bytes, &buffer_end) ||
            buffer_end > buffers[indices_view->buffer].len ||
            !gltf_checked_add_size(value_buffer_offset, value_bytes, &buffer_end) ||
            buffer_end > buffers[values_view->buffer].len)
            return 0;
        out->sparse_indices = buffers[indices_view->buffer].data + index_buffer_offset;
        out->sparse_values = buffers[values_view->buffer].data + value_buffer_offset;
        out->sparse_count = (int32_t)accessor->sparse_count;
        out->sparse_index_comp_type = accessor->sparse_index_comp_type;
        out->sparse_index_stride = index_comp_size;
        out->sparse_value_stride = (int32_t)element_size;
        for (int32_t si = 0; si < out->sparse_count; si++) {
            uint32_t sparse_index = gltf_decode_component_u32(
                out->sparse_indices + (size_t)si * (size_t)out->sparse_index_stride,
                out->sparse_index_comp_type);
            if (sparse_index >= (uint32_t)out->count ||
                (si > 0 && sparse_index <= previous_sparse_index))
                return 0;
            previous_sparse_index = sparse_index;
        }
    }
    return 1;
}

/// @brief Whether all @p count floats are finite (no NaN/Inf); false for NULL or non-positive
/// count.
/// @param[in] values Floating-point sequence to inspect.
/// @param count Number of elements in @p values.
/// @return Non-zero only when @p values is non-null, @p count is positive, and every
///         element is finite.
static int gltf_preload_floats_are_finite(const float *values, int count) {
    if (!values || count <= 0)
        return 0;
    for (int i = 0; i < count; i++) {
        if (!isfinite(values[i]))
            return 0;
    }
    return 1;
}

/// @brief Whether three vertices form a non-degenerate triangle (positive area).
/// @details Computes the squared area from the cross product of two edges and rejects values
///          below 1e-20, dropping zero-area triangles that would produce invalid normals.
/// @param[in] vertices Packed vertex array.
/// @param vertex_count Number of entries in @p vertices.
/// @param i0 First candidate vertex index.
/// @param i1 Second candidate vertex index.
/// @param i2 Third candidate vertex index.
/// @return Non-zero when all indices are in range and their positions form a finite
///         triangle above the degeneracy threshold.
static int gltf_preload_pod_positions_form_triangle(
    const vgfx3d_vertex_t *vertices, uint32_t vertex_count, uint32_t i0, uint32_t i1, uint32_t i2) {
    const float *p0;
    const float *p1;
    const float *p2;
    double e1[3];
    double e2[3];
    double nx;
    double ny;
    double nz;
    double area_sq;
    if (!vertices || i0 >= vertex_count || i1 >= vertex_count || i2 >= vertex_count)
        return 0;
    p0 = vertices[i0].pos;
    p1 = vertices[i1].pos;
    p2 = vertices[i2].pos;
    e1[0] = (double)p1[0] - (double)p0[0];
    e1[1] = (double)p1[1] - (double)p0[1];
    e1[2] = (double)p1[2] - (double)p0[2];
    e2[0] = (double)p2[0] - (double)p0[0];
    e2[1] = (double)p2[1] - (double)p0[1];
    e2[2] = (double)p2[2] - (double)p0[2];
    nx = e1[1] * e2[2] - e1[2] * e2[1];
    ny = e1[2] * e2[0] - e1[0] * e2[2];
    nz = e1[0] * e2[1] - e1[1] * e2[0];
    area_sq = nx * nx + ny * ny + nz * nz;
    return isfinite(area_sq) && area_sq > 1e-20;
}

/// @brief Append a triangle's three indices if it is non-degenerate (distinct, positive-area).
/// @param[in] vertices Packed vertex array used for geometric validation.
/// @param vertex_count Number of entries in @p vertices.
/// @param i0 First candidate vertex index.
/// @param i1 Second candidate vertex index.
/// @param i2 Third candidate vertex index.
/// @param[out] indices Output index array with capacity for three additional entries.
/// @param[in,out] index_count Number of populated output indices, advanced by three on success.
/// @return Non-zero if emitted; zero if the triangle has repeated, out-of-range, or
///         geometrically degenerate vertices.
static int gltf_preload_emit_pod_triangle(const vgfx3d_vertex_t *vertices,
                                          uint32_t vertex_count,
                                          uint32_t i0,
                                          uint32_t i1,
                                          uint32_t i2,
                                          uint32_t *indices,
                                          uint32_t *index_count) {
    if (!indices || !index_count || i0 == i1 || i1 == i2 || i0 == i2)
        return 0;
    if (!gltf_preload_pod_positions_form_triangle(vertices, vertex_count, i0, i1, i2))
        return 0;
    indices[(*index_count)++] = i0;
    indices[(*index_count)++] = i1;
    indices[(*index_count)++] = i2;
    return 1;
}

/// @brief Read an index from the index accessor, or use @p element_index directly for non-indexed
/// geometry.
/// @param[in] view Optional index accessor view.
/// @param element_index Logical source index element.
/// @return Decoded accessor value, or @p element_index when @p view is `NULL`.
static uint32_t gltf_preload_read_index_or_vertex(const gltf_accessor_view_t *view,
                                                  int32_t element_index) {
    uint32_t value = (uint32_t)element_index;
    if (view)
        gltf_accessor_read_u32(view, element_index, &value, 1);
    return value;
}

/// @brief Compute the triangle-list index capacity needed to triangulate a primitive topology.
/// @details mode 4 = triangles (count/3 tris), 5/6 = strip/fan (count-2 tris); each yields 3
///          indices per triangle. Overflow-checked. Returns 0 for unsupported topologies.
/// @param mode glTF primitive topology mode.
/// @param source_index_count Number of source indices or implicit vertices.
/// @param[out] out_capacity Receives the maximum emitted triangle-list index count.
/// @return Non-zero when the topology is supported and its capacity is representable;
///         zero for an unsupported mode or overflow.
static int gltf_preload_topology_index_capacity(int mode,
                                                int32_t source_index_count,
                                                uint32_t *out_capacity) {
    size_t tri_count;
    size_t index_capacity;
    if (out_capacity)
        *out_capacity = 0;
    if (source_index_count < 3 || !out_capacity)
        return 1;
    if (mode == 4)
        tri_count = (size_t)(source_index_count / 3);
    else if (mode == 5 || mode == 6)
        tri_count = (size_t)(source_index_count - 2);
    else
        return 0;
    if (!gltf_checked_mul_size(tri_count, 3u, &index_capacity) || index_capacity > UINT32_MAX) {
        return 0;
    }
    *out_capacity = (uint32_t)index_capacity;
    return 1;
}

/// @brief Write a vertex's 4 skinning joints/weights, normalizing the weights to sum to 1.
/// @details Out-of-range joints and non-positive weights are zeroed; weights are renormalized by
///          their total (or all zeroed if the total is ~0).
/// @param[out] vertex Packed vertex receiving sanitized joint indices and normalized weights.
/// @param[in] joints Four source joint indices.
/// @param[in] weights Four source joint weights.
/// @return One past the highest referenced bone index (i.e. a bone-count hint), or 0 if none.
static uint32_t gltf_preload_write_vertex_bone_weights(vgfx3d_vertex_t *vertex,
                                                       const uint32_t joints[4],
                                                       const float weights[4]) {
    double sum = 0.0;
    int32_t max_bone_index = -1;
    if (!vertex || !joints || !weights)
        return 0u;
    for (int i = 0; i < 4; i++) {
        if (joints[i] >= VGFX3D_MAX_BONES) {
            vertex->bone_indices[i] = 0u;
            vertex->bone_weights[i] = 0.0f;
            continue;
        }
        vertex->bone_indices[i] = (uint8_t)joints[i];
        if (isfinite(weights[i]) && weights[i] > 0.0f) {
            vertex->bone_weights[i] = weights[i];
            sum += (double)weights[i];
            if ((int32_t)joints[i] > max_bone_index)
                max_bone_index = (int32_t)joints[i];
        } else {
            vertex->bone_weights[i] = 0.0f;
        }
    }
    if (sum > 1e-12) {
        for (int i = 0; i < 4; i++)
            vertex->bone_weights[i] = (float)((double)vertex->bone_weights[i] / sum);
    } else {
        for (int i = 0; i < 4; i++)
            vertex->bone_weights[i] = 0.0f;
    }
    return max_bone_index >= 0 ? (uint32_t)(max_bone_index + 1) : 0u;
}

/// @brief Serialize a packed mesh primitive into a "VGMP" POD blob and stage it as a dependency.
/// @details Writes the 40-byte header (magic, version, vertex/index counts, flags, byte spans, bone
///          count, skin-import diagnostics) followed by the raw vertex and index arrays, all
///          overflow-checked, then stores it under the mesh/primitive key for
///          gltf_preload_take_decoded_mesh to consume later.
/// @param[in,out] bundle Bundle receiving the owned mesh POD dependency.
/// @param mesh_index Zero-based source mesh index used in the dependency key.
/// @param primitive_index Zero-based source primitive index used in the dependency key.
/// @param flags Packed mesh attribute-presence flags.
/// @param[in] vertices Packed vertex array.
/// @param vertex_count Number of entries in @p vertices.
/// @param[in] indices Triangle-list index array.
/// @param index_count Number of entries in @p indices.
/// @param bone_count Highest referenced bone count for the primitive.
/// @param[in] skin_stats Optional skin truncation and out-of-range counters.
/// @param[out] error Optional caller-provided error buffer.
/// @param error_cap Capacity of @p error in bytes.
/// @return Non-zero when the POD was staged or the primitive is intentionally omitted;
///         zero on blob allocation or dependency insertion failure.
static int gltf_preload_pack_mesh_pod(rt_gltf_preload_bundle *bundle,
                                      int mesh_index,
                                      int primitive_index,
                                      uint32_t flags,
                                      vgfx3d_vertex_t *vertices,
                                      uint32_t vertex_count,
                                      uint32_t *indices,
                                      uint32_t index_count,
                                      uint32_t bone_count,
                                      const gltf_skin_import_stats_t *skin_stats,
                                      char *error,
                                      size_t error_cap) {
    uint8_t *blob;
    size_t vertex_bytes;
    size_t index_bytes;
    size_t payload_offset;
    size_t blob_len;
    char key[96];
    if (!vertices || !indices || vertex_count == 0 || index_count == 0)
        return 1;
    if (bone_count > VGFX3D_MAX_BONES)
        return 1;
    if (!gltf_checked_mul_size((size_t)vertex_count, sizeof(vgfx3d_vertex_t), &vertex_bytes) ||
        !gltf_checked_mul_size((size_t)index_count, sizeof(uint32_t), &index_bytes) ||
        !gltf_checked_add_size(GLTF_PRELOAD_MESH_POD_HEADER_SIZE, vertex_bytes, &payload_offset) ||
        !gltf_checked_add_size(payload_offset, index_bytes, &blob_len) ||
        vertex_bytes > UINT32_MAX || index_bytes > UINT32_MAX) {
        return 1;
    }
    blob = (uint8_t *)malloc(blob_len);
    if (!blob) {
        gltf_preload_set_error(error, error_cap, "failed to stage glTF dependency");
        return 0;
    }
    memset(blob, 0, GLTF_PRELOAD_MESH_POD_HEADER_SIZE);
    gltf_write_u32_le(blob + 0, GLTF_PRELOAD_MESH_POD_MAGIC);
    gltf_write_u32_le(blob + 4, GLTF_PRELOAD_MESH_POD_VERSION);
    gltf_write_u32_le(blob + 8, vertex_count);
    gltf_write_u32_le(blob + 12, index_count);
    gltf_write_u32_le(blob + 16, flags);
    gltf_write_u32_le(blob + 20, (uint32_t)vertex_bytes);
    gltf_write_u32_le(blob + 24, (uint32_t)index_bytes);
    gltf_write_u32_le(blob + 28, bone_count);
    gltf_write_u32_le(blob + 32, skin_stats ? skin_stats->truncated_vertices : 0u);
    gltf_write_u32_le(blob + 36, skin_stats ? skin_stats->out_of_range_vertices : 0u);
    memcpy(blob + GLTF_PRELOAD_MESH_POD_HEADER_SIZE, vertices, vertex_bytes);
    memcpy(blob + payload_offset, indices, index_bytes);
    gltf_preload_mesh_key(mesh_index, primitive_index, key, sizeof(key));
    if (!gltf_preload_bundle_add_dependency(
            bundle, key, GLTF_PRELOAD_DEP_MESH_POD, blob, blob_len)) {
        free(blob);
        gltf_preload_set_error(error, error_cap, "failed to stage glTF dependency");
        return 0;
    }
    return 1;
}

/// @brief One decoded morph shape staged for packing: its name, base weight, present-channel
///        flags, and owned per-vertex position/normal/tangent delta arrays.
typedef struct {
    /// Owned UTF-8 display name.
    char *name;
    /// Sanitized default morph weight.
    float weight;
    /// `GLTF_PRELOAD_MORPH_POD_HAS_*` channel-presence mask.
    uint32_t flags;
    /// Owned position deltas, three floats per base vertex.
    float *pos_deltas;
    /// Owned normal deltas, three floats per base vertex.
    float *normal_deltas;
    /// Owned tangent deltas, three floats per base vertex.
    float *tangent_deltas;
} gltf_preload_morph_shape_t;

/// @brief Free an array of morph shapes and every owned name/delta buffer inside it.
/// @param[in,out] shapes Owned shape array to release; `NULL` is accepted.
/// @param shape_count Number of initialized entries in @p shapes.
static void gltf_preload_free_morph_shapes(gltf_preload_morph_shape_t *shapes, size_t shape_count) {
    if (!shapes)
        return;
    for (size_t i = 0; i < shape_count; i++) {
        free(shapes[i].name);
        free(shapes[i].pos_deltas);
        free(shapes[i].normal_deltas);
        free(shapes[i].tangent_deltas);
    }
    free(shapes);
}

/// @brief Resolve a morph target's display name, falling back to "target_N".
/// @details Reads the optional mesh `extras.targetNames[target_index]`; if absent or empty,
///          synthesizes a name from the index. Returns an owned string (caller frees).
/// @param[in] json Complete glTF JSON text.
/// @param json_len Length of @p json in bytes.
/// @param mesh_start Inclusive byte offset of the parent mesh object.
/// @param mesh_end Exclusive byte offset bounding the parent mesh object.
/// @param target_index Zero-based morph target index.
/// @return Heap-owned target name, or `NULL` if allocation fails.
static char *gltf_preload_target_name_alloc(
    const char *json, size_t json_len, size_t mesh_start, size_t mesh_end, int target_index) {
    size_t extras_start;
    size_t extras_end;
    size_t names_start;
    size_t names_end;
    char fallback[64];
    char *name = NULL;
    if (gltf_json_object_find_value(
            json, json_len, mesh_start, mesh_end, "extras", &extras_start, &extras_end) &&
        extras_start < extras_end && json[extras_start] == '{' &&
        gltf_json_object_find_value(
            json, json_len, extras_start, extras_end, "targetNames", &names_start, &names_end) &&
        names_start < names_end && json[names_start] == '[') {
        name =
            gltf_json_array_get_string_alloc(json, json_len, names_start, names_end, target_index);
    }
    if (name && name[0] != '\0')
        return name;
    free(name);
    snprintf(fallback, sizeof(fallback), "target_%d", target_index);
    return gltf_strdup_cstr(fallback);
}

/// @brief Read a morph target's default weight from the mesh "weights" array (0.0 if absent).
/// @param[in] json Complete glTF JSON text.
/// @param json_len Length of @p json in bytes.
/// @param mesh_start Inclusive byte offset of the parent mesh object.
/// @param mesh_end Exclusive byte offset bounding the parent mesh object.
/// @param target_index Zero-based morph target index.
/// @return Finite default weight clamped to [-1, 1], or zero when absent or invalid.
static double gltf_preload_target_weight(
    const char *json, size_t json_len, size_t mesh_start, size_t mesh_end, int target_index) {
    size_t weights_start;
    size_t weights_end;
    double weight;
    if (!gltf_json_object_find_value(
            json, json_len, mesh_start, mesh_end, "weights", &weights_start, &weights_end) ||
        weights_start >= weights_end || json[weights_start] != '[')
        return 0.0;
    weight =
        gltf_json_array_get_number(json, json_len, weights_start, weights_end, target_index, 0.0);
    if (!isfinite(weight))
        return 0.0;
    if (weight < -1.0)
        return -1.0;
    if (weight > 1.0)
        return 1.0;
    return weight;
}

/// @brief Copy one morph channel (vec3 deltas) from an accessor view into an owned float array.
/// @details Allocates vertex_count*3 floats, samples up to the accessor's count (zero-padding the
///          rest), and sanitizes non-finite components to 0. NULL view yields a NULL out (no-op).
/// @param[in] view Optional resolved vec3 delta accessor.
/// @param vertex_count Number of base-mesh vertices to allocate.
/// @param[out] out_deltas Receives the owned, zero-padded delta array.
/// @return Non-zero on success or when no view is present; zero on size overflow or
///         allocation failure.
static int gltf_preload_copy_morph_channel(const gltf_accessor_view_t *view,
                                           int32_t vertex_count,
                                           float **out_deltas) {
    size_t bytes;
    float *deltas;
    int32_t limit;
    if (out_deltas)
        *out_deltas = NULL;
    if (!view || !out_deltas || vertex_count <= 0)
        return 1;
    if (!gltf_checked_mul_size((size_t)vertex_count, 3u * sizeof(float), &bytes))
        return 0;
    deltas = (float *)calloc(1, bytes);
    if (!deltas)
        return 0;
    limit = view->count < vertex_count ? view->count : vertex_count;
    for (int32_t vi = 0; vi < limit; vi++) {
        float sample[3] = {0.0f, 0.0f, 0.0f};
        gltf_accessor_read_f32(view, vi, sample, 3);
        for (int c = 0; c < 3; c++)
            deltas[(size_t)vi * 3u + (size_t)c] = isfinite(sample[c]) ? sample[c] : 0.0f;
    }
    *out_deltas = deltas;
    return 1;
}

/// @brief Serialize a primitive's morph shapes into a "VGMT" POD blob and stage it as a dependency.
/// @details Lays out a header, fixed-size per-shape records, a packed names region, and a deltas
///          payload (position/normal/tangent channels per the flags), all overflow-checked, then
///          stores it under the morph key for gltf_preload_take_decoded_morph to consume later.
/// @param[in,out] bundle Bundle receiving the owned morph POD dependency.
/// @param mesh_index Zero-based source mesh index used in the dependency key.
/// @param primitive_index Zero-based primitive index used in the dependency key.
/// @param[in] shapes Decoded morph-shape array.
/// @param shape_count Number of entries in @p shapes.
/// @param vertex_count Number of base vertices represented by every channel.
/// @param[out] error Optional caller-provided error buffer.
/// @param error_cap Capacity of @p error in bytes.
/// @return Non-zero when the POD is staged or there is nothing to pack; zero on
///         invalid representable size, allocation, or dependency insertion failure.
static int gltf_preload_pack_morph_pod(rt_gltf_preload_bundle *bundle,
                                       int mesh_index,
                                       int primitive_index,
                                       const gltf_preload_morph_shape_t *shapes,
                                       size_t shape_count,
                                       uint32_t vertex_count,
                                       char *error,
                                       size_t error_cap) {
    size_t channel_bytes;
    size_t record_bytes;
    size_t names_bytes = 0;
    size_t payload_bytes = 0;
    size_t names_offset;
    size_t payload_offset;
    size_t blob_len;
    size_t name_cursor = 0;
    size_t payload_cursor = 0;
    uint8_t *blob;
    char key[96];
    if (!bundle || !shapes || shape_count == 0 || vertex_count == 0)
        return 1;
    if (shape_count > INT32_MAX || vertex_count > INT32_MAX) {
        gltf_preload_set_error(error, error_cap, "glTF morph payload is too large");
        return 0;
    }
    if (!gltf_checked_mul_size((size_t)vertex_count, 3u * sizeof(float), &channel_bytes) ||
        !gltf_checked_mul_size(shape_count, GLTF_PRELOAD_MORPH_POD_RECORD_SIZE, &record_bytes)) {
        gltf_preload_set_error(error, error_cap, "glTF morph payload is too large");
        return 0;
    }
    for (size_t i = 0; i < shape_count; i++) {
        size_t name_len = shapes[i].name ? strlen(shapes[i].name) : 0u;
        if (name_len > SIZE_MAX - 1u) {
            gltf_preload_set_error(error, error_cap, "glTF morph target name is too large");
            return 0;
        }
        name_len += 1u;
        if (!gltf_checked_add_size(names_bytes, name_len, &names_bytes)) {
            gltf_preload_set_error(error, error_cap, "glTF morph payload is too large");
            return 0;
        }
        if ((shapes[i].flags & GLTF_PRELOAD_MORPH_POD_HAS_POSITIONS) != 0 &&
            !gltf_checked_add_size(payload_bytes, channel_bytes, &payload_bytes)) {
            gltf_preload_set_error(error, error_cap, "glTF morph payload is too large");
            return 0;
        }
        if ((shapes[i].flags & GLTF_PRELOAD_MORPH_POD_HAS_NORMALS) != 0 &&
            !gltf_checked_add_size(payload_bytes, channel_bytes, &payload_bytes)) {
            gltf_preload_set_error(error, error_cap, "glTF morph payload is too large");
            return 0;
        }
        if ((shapes[i].flags & GLTF_PRELOAD_MORPH_POD_HAS_TANGENTS) != 0 &&
            !gltf_checked_add_size(payload_bytes, channel_bytes, &payload_bytes)) {
            gltf_preload_set_error(error, error_cap, "glTF morph payload is too large");
            return 0;
        }
    }
    if (!gltf_checked_add_size(GLTF_PRELOAD_MORPH_POD_HEADER_SIZE, record_bytes, &names_offset) ||
        !gltf_checked_add_size(names_offset, names_bytes, &payload_offset) ||
        !gltf_checked_add_size(payload_offset, payload_bytes, &blob_len) ||
        record_bytes > UINT32_MAX || names_bytes > UINT32_MAX || payload_bytes > UINT32_MAX) {
        gltf_preload_set_error(error, error_cap, "glTF morph payload is too large");
        return 0;
    }
    blob = (uint8_t *)calloc(1, blob_len);
    if (!blob) {
        gltf_preload_set_error(error, error_cap, "failed to stage glTF dependency");
        return 0;
    }
    gltf_write_u32_le(blob + 0, GLTF_PRELOAD_MORPH_POD_MAGIC);
    gltf_write_u32_le(blob + 4, GLTF_PRELOAD_MORPH_POD_VERSION);
    gltf_write_u32_le(blob + 8, vertex_count);
    gltf_write_u32_le(blob + 12, (uint32_t)shape_count);
    gltf_write_u32_le(blob + 16, (uint32_t)record_bytes);
    gltf_write_u32_le(blob + 20, (uint32_t)names_bytes);
    gltf_write_u32_le(blob + 24, (uint32_t)payload_bytes);
    gltf_write_u32_le(blob + 28, 0u);

    for (size_t i = 0; i < shape_count; i++) {
        uint8_t *record =
            blob + GLTF_PRELOAD_MORPH_POD_HEADER_SIZE + i * GLTF_PRELOAD_MORPH_POD_RECORD_SIZE;
        const char *name = shapes[i].name ? shapes[i].name : "";
        size_t name_len = strlen(name) + 1u;
        uint32_t pos_offset = 0u;
        uint32_t norm_offset = 0u;
        uint32_t tan_offset = 0u;
        if ((shapes[i].flags & GLTF_PRELOAD_MORPH_POD_HAS_POSITIONS) != 0) {
            pos_offset = (uint32_t)payload_cursor;
            memcpy(blob + payload_offset + payload_cursor, shapes[i].pos_deltas, channel_bytes);
            payload_cursor += channel_bytes;
        }
        if ((shapes[i].flags & GLTF_PRELOAD_MORPH_POD_HAS_NORMALS) != 0) {
            norm_offset = (uint32_t)payload_cursor;
            memcpy(blob + payload_offset + payload_cursor, shapes[i].normal_deltas, channel_bytes);
            payload_cursor += channel_bytes;
        }
        if ((shapes[i].flags & GLTF_PRELOAD_MORPH_POD_HAS_TANGENTS) != 0) {
            tan_offset = (uint32_t)payload_cursor;
            memcpy(blob + payload_offset + payload_cursor, shapes[i].tangent_deltas, channel_bytes);
            payload_cursor += channel_bytes;
        }
        memcpy(blob + names_offset + name_cursor, name, name_len);
        gltf_write_u32_le(record + 0, shapes[i].flags);
        gltf_write_u32_le(record + 4, (uint32_t)name_cursor);
        gltf_write_u32_le(record + 8, (uint32_t)name_len);
        gltf_write_f32_le(record + 12, shapes[i].weight);
        gltf_write_u32_le(record + 16, pos_offset);
        gltf_write_u32_le(record + 20, norm_offset);
        gltf_write_u32_le(record + 24, tan_offset);
        gltf_write_u32_le(record + 28, 0u);
        name_cursor += name_len;
    }

    gltf_preload_morph_key(mesh_index, primitive_index, key, sizeof(key));
    if (!gltf_preload_bundle_add_dependency(
            bundle, key, GLTF_PRELOAD_DEP_MORPH_POD, blob, blob_len)) {
        free(blob);
        gltf_preload_set_error(error, error_cap, "failed to stage glTF dependency");
        return 0;
    }
    return 1;
}

/// @brief Decode a primitive's morph targets and stage them as a morph POD dependency.
/// @details Parses the primitive's "targets" JSON array; for each entry resolves the optional
///          POSITION/NORMAL/TANGENT accessor views, copies their per-vertex deltas into a
///          growing morph-shape list (tagging present channels via GLTF_PRELOAD_MORPH_POD_HAS_*
///          and recording each target's name and morph weight), then hands the collected shapes
///          to gltf_preload_pack_morph_pod so gltf_preload_take_decoded_morph can rehydrate them
///          at load time. A primitive without a "targets" array is a successful no-op.
/// @param[in,out] bundle Bundle receiving the packed morph dependency.
/// @param[in] json Complete glTF JSON text.
/// @param json_len Length of @p json in bytes.
/// @param mesh_index Zero-based source mesh index.
/// @param primitive_index Zero-based primitive index within the source mesh.
/// @param mesh_start Inclusive byte offset of the parent mesh object.
/// @param mesh_end Exclusive byte offset bounding the parent mesh object.
/// @param prim_start Inclusive byte offset of the primitive object.
/// @param prim_end Exclusive byte offset bounding the primitive object.
/// @param[in] buffers Staged buffer-reference table.
/// @param buffer_count Number of entries in @p buffers.
/// @param[in] views Parsed buffer-view-reference table.
/// @param view_count Number of entries in @p views.
/// @param[in] accessors Parsed accessor-reference table.
/// @param accessor_count Number of entries in @p accessors.
/// @param vertex_count Number of base vertices represented by each target.
/// @param[out] error Optional caller-provided error buffer.
/// @param error_cap Capacity of @p error in bytes.
/// @return Non-zero on success or when there is nothing to stage; zero on allocation
///         or dependency-staging failure.
static int gltf_preload_stage_morph_targets(rt_gltf_preload_bundle *bundle,
                                            const char *json,
                                            size_t json_len,
                                            int mesh_index,
                                            int primitive_index,
                                            size_t mesh_start,
                                            size_t mesh_end,
                                            size_t prim_start,
                                            size_t prim_end,
                                            const gltf_preload_buffer_ref_t *buffers,
                                            int buffer_count,
                                            const gltf_preload_buffer_view_ref_t *views,
                                            int view_count,
                                            const gltf_preload_accessor_ref_t *accessors,
                                            int accessor_count,
                                            int32_t vertex_count,
                                            char *error,
                                            size_t error_cap) {
    size_t targets_start;
    size_t targets_end;
    size_t target_pos;
    gltf_preload_morph_shape_t *shapes = NULL;
    size_t shape_count = 0;
    int target_index = 0;
    int ok = 1;
    if (!bundle || !json || vertex_count <= 0)
        return 1;
    if (!gltf_json_object_find_value(
            json, json_len, prim_start, prim_end, "targets", &targets_start, &targets_end) ||
        targets_start >= targets_end || json[targets_start] != '[')
        return 1;
    target_pos = targets_start + 1u;
    while (target_pos < targets_end) {
        target_pos = gltf_json_skip_ws(json, json_len, target_pos);
        if (target_pos >= targets_end || json[target_pos] == ']')
            break;
        if (json[target_pos] == '{') {
            size_t target_end = gltf_json_find_matching(json, json_len, target_pos, '{', '}');
            int pos_acc;
            int norm_acc;
            int tangent_acc;
            int has_pos;
            int has_norm;
            int has_tangent;
            gltf_accessor_view_t pos_view;
            gltf_accessor_view_t norm_view;
            gltf_accessor_view_t tangent_view;
            if (target_end == SIZE_MAX || target_end > targets_end)
                break;
            pos_acc =
                gltf_json_object_get_int(json, json_len, target_pos, target_end, "POSITION", -1);
            norm_acc =
                gltf_json_object_get_int(json, json_len, target_pos, target_end, "NORMAL", -1);
            tangent_acc =
                gltf_json_object_get_int(json, json_len, target_pos, target_end, "TANGENT", -1);
            has_pos = gltf_preload_resolve_accessor_view(accessors,
                                                         accessor_count,
                                                         pos_acc,
                                                         buffers,
                                                         buffer_count,
                                                         views,
                                                         view_count,
                                                         &pos_view) &&
                      gltf_accessor_has_payload(&pos_view) && gltf_accessor_is_position(&pos_view);
            has_norm = gltf_preload_resolve_accessor_view(accessors,
                                                          accessor_count,
                                                          norm_acc,
                                                          buffers,
                                                          buffer_count,
                                                          views,
                                                          view_count,
                                                          &norm_view) &&
                       gltf_accessor_has_payload(&norm_view) && gltf_accessor_is_normal(&norm_view);
            has_tangent = gltf_preload_resolve_accessor_view(accessors,
                                                             accessor_count,
                                                             tangent_acc,
                                                             buffers,
                                                             buffer_count,
                                                             views,
                                                             view_count,
                                                             &tangent_view) &&
                          gltf_accessor_has_payload(&tangent_view) &&
                          gltf_accessor_is_tangent(&tangent_view, 3);
            if (has_pos || has_norm || has_tangent) {
                if (shape_count > (SIZE_MAX / sizeof(*shapes)) - 1u) {
                    gltf_preload_set_error(error, error_cap, "failed to stage glTF dependency");
                    ok = 0;
                    goto done;
                }
                gltf_preload_morph_shape_t *grown = (gltf_preload_morph_shape_t *)realloc(
                    shapes, (shape_count + 1u) * sizeof(*shapes));
                if (!grown) {
                    gltf_preload_set_error(error, error_cap, "failed to stage glTF dependency");
                    ok = 0;
                    goto done;
                }
                shapes = grown;
                memset(&shapes[shape_count], 0, sizeof(shapes[shape_count]));
                shapes[shape_count].name = gltf_preload_target_name_alloc(
                    json, json_len, mesh_start, mesh_end, target_index);
                if (!shapes[shape_count].name) {
                    gltf_preload_set_error(error, error_cap, "failed to stage glTF dependency");
                    ok = 0;
                    goto done;
                }
                shapes[shape_count].weight = (float)gltf_preload_target_weight(
                    json, json_len, mesh_start, mesh_end, target_index);
                if (has_pos) {
                    if (!gltf_preload_copy_morph_channel(
                            &pos_view, vertex_count, &shapes[shape_count].pos_deltas)) {
                        gltf_preload_set_error(error, error_cap, "failed to stage glTF dependency");
                        ok = 0;
                        goto done;
                    }
                    shapes[shape_count].flags |= GLTF_PRELOAD_MORPH_POD_HAS_POSITIONS;
                }
                if (has_norm) {
                    if (!gltf_preload_copy_morph_channel(
                            &norm_view, vertex_count, &shapes[shape_count].normal_deltas)) {
                        gltf_preload_set_error(error, error_cap, "failed to stage glTF dependency");
                        ok = 0;
                        goto done;
                    }
                    shapes[shape_count].flags |= GLTF_PRELOAD_MORPH_POD_HAS_NORMALS;
                }
                if (has_tangent) {
                    if (!gltf_preload_copy_morph_channel(
                            &tangent_view, vertex_count, &shapes[shape_count].tangent_deltas)) {
                        gltf_preload_set_error(error, error_cap, "failed to stage glTF dependency");
                        ok = 0;
                        goto done;
                    }
                    shapes[shape_count].flags |= GLTF_PRELOAD_MORPH_POD_HAS_TANGENTS;
                }
                shape_count++;
            }
            target_index++;
            target_pos = target_end;
        } else {
            target_pos = gltf_json_skip_value(json, json_len, target_pos);
            if (target_pos == SIZE_MAX)
                break;
        }
        target_pos = gltf_json_skip_ws(json, json_len, target_pos);
        if (target_pos < targets_end && json[target_pos] == ',')
            target_pos++;
    }
    ok = gltf_preload_pack_morph_pod(bundle,
                                     mesh_index,
                                     primitive_index,
                                     shapes,
                                     shape_count,
                                     (uint32_t)vertex_count,
                                     error,
                                     error_cap);

done:
    gltf_preload_free_morph_shapes(shapes, shape_count);
    return ok;
}

/// @brief Resolved POD-staging state for one glTF mesh primitive: every attribute's
///        accessor view + presence flag, plus derived vertex/index counts and topology.
typedef struct {
    /// glTF topology mode: triangle list, strip, or fan.
    int mode;
    /// Required position accessor.
    gltf_accessor_view_t pos_view;
    /// Optional normal accessor.
    gltf_accessor_view_t norm_view;
    /// Optional primary texture-coordinate accessor.
    gltf_accessor_view_t uv0_view;
    /// Optional secondary texture-coordinate accessor.
    gltf_accessor_view_t uv1_view;
    /// Optional color accessor.
    gltf_accessor_view_t color_view;
    /// Optional tangent accessor.
    gltf_accessor_view_t tangent_view;
    /// Optional first joint-index accessor.
    gltf_accessor_view_t joints_view;
    /// Optional first joint-weight accessor.
    gltf_accessor_view_t weights_view;
    /// Optional second joint-index accessor.
    gltf_accessor_view_t joints1_view;
    /// Optional second joint-weight accessor.
    gltf_accessor_view_t weights1_view;
    /// Optional primitive index accessor.
    gltf_accessor_view_t idx_view;
    /// Whether @ref norm_view covers all vertices with a usable format.
    int has_normals;
    /// Whether @ref uv0_view covers all vertices with a usable format.
    int has_uv0;
    /// Whether @ref uv1_view covers all vertices with a usable format.
    int has_uv1;
    /// Whether @ref color_view covers all vertices with a usable format.
    int has_colors;
    /// Whether @ref tangent_view covers all vertices with a usable format.
    int has_tangents;
    /// Whether the first joint-index stream is usable.
    int has_joints;
    /// Whether the first joint-weight stream is usable.
    int has_weights;
    /// Whether the second joint-index stream is usable.
    int has_joints1;
    /// Whether the second joint-weight stream is usable.
    int has_weights1;
    /// Whether at least one complete joint/weight pair is present.
    int has_skinning_attrs;
    /// Whether a supported scalar index accessor is present.
    int has_indices;
    /// Number of source position elements.
    int32_t vertex_count_i;
    /// Number of explicit indices or implicit vertex indices to triangulate.
    int32_t source_index_count;
    /// Maximum number of triangle-list indices required after topology expansion.
    uint32_t index_capacity_count;
} gltf_primitive_attribs_t;

/// @brief Resolve every attribute/index accessor view of a primitive into @p out.
/// @param[in] json Complete glTF JSON text.
/// @param json_len Length of @p json in bytes.
/// @param prim_start Inclusive byte offset of the primitive object.
/// @param prim_end Exclusive byte offset bounding the primitive object.
/// @param[in] buffers Staged buffer-reference table.
/// @param buffer_count Number of entries in @p buffers.
/// @param[in] views Parsed buffer-view-reference table.
/// @param view_count Number of entries in @p views.
/// @param[in] accessors Parsed accessor-reference table.
/// @param accessor_count Number of entries in @p accessors.
/// @param[out] out Resolved primitive attribute state.
/// @return Non-zero when the primitive has supported topology, valid positions, paired
///         skin streams, and a representable index capacity; zero to skip it.
static int gltf_preload_resolve_primitive_attribs(const char *json,
                                                  size_t json_len,
                                                  size_t prim_start,
                                                  size_t prim_end,
                                                  const gltf_preload_buffer_ref_t *buffers,
                                                  int buffer_count,
                                                  const gltf_preload_buffer_view_ref_t *views,
                                                  int view_count,
                                                  const gltf_preload_accessor_ref_t *accessors,
                                                  int accessor_count,
                                                  gltf_primitive_attribs_t *out) {
    size_t attrs_start;
    size_t attrs_end;
    int pos_acc;
    int norm_acc;
    int uv0_acc;
    int uv1_acc;
    int color_acc;
    int tangent_acc;
    int joints_acc;
    int weights_acc;
    int joints1_acc;
    int weights1_acc;
    int idx_acc;

    memset(out, 0, sizeof(*out));
    if (!gltf_json_object_find_value(
            json, json_len, prim_start, prim_end, "attributes", &attrs_start, &attrs_end) ||
        attrs_start >= attrs_end || json[attrs_start] != '{')
        return 0;
    out->mode = gltf_json_object_get_int(json, json_len, prim_start, prim_end, "mode", 4);
    if (out->mode != 4 && out->mode != 5 && out->mode != 6)
        return 0;

    pos_acc = gltf_json_object_get_int(json, json_len, attrs_start, attrs_end, "POSITION", -1);
    norm_acc = gltf_json_object_get_int(json, json_len, attrs_start, attrs_end, "NORMAL", -1);
    if (pos_acc < 0)
        return 0;

    if (!gltf_preload_resolve_accessor_view(accessors,
                                            accessor_count,
                                            pos_acc,
                                            buffers,
                                            buffer_count,
                                            views,
                                            view_count,
                                            &out->pos_view))
        return 0;
    out->has_normals = gltf_preload_resolve_accessor_view(accessors,
                                                          accessor_count,
                                                          norm_acc,
                                                          buffers,
                                                          buffer_count,
                                                          views,
                                                          view_count,
                                                          &out->norm_view) &&
                       out->norm_view.comp_count >= 3 &&
                       out->norm_view.count >= out->pos_view.count;
    if (out->pos_view.comp_count < 3 || out->pos_view.count <= 0)
        return 0;

    uv0_acc = gltf_json_object_get_int(json, json_len, attrs_start, attrs_end, "TEXCOORD_0", -1);
    uv1_acc = gltf_json_object_get_int(json, json_len, attrs_start, attrs_end, "TEXCOORD_1", -1);
    color_acc = gltf_json_object_get_int(json, json_len, attrs_start, attrs_end, "COLOR_0", -1);
    tangent_acc = gltf_json_object_get_int(json, json_len, attrs_start, attrs_end, "TANGENT", -1);
    joints_acc = gltf_json_object_get_int(json, json_len, attrs_start, attrs_end, "JOINTS_0", -1);
    weights_acc = gltf_json_object_get_int(json, json_len, attrs_start, attrs_end, "WEIGHTS_0", -1);
    joints1_acc = gltf_json_object_get_int(json, json_len, attrs_start, attrs_end, "JOINTS_1", -1);
    weights1_acc =
        gltf_json_object_get_int(json, json_len, attrs_start, attrs_end, "WEIGHTS_1", -1);
    idx_acc = gltf_json_object_get_int(json, json_len, prim_start, prim_end, "indices", -1);

    out->has_uv0 = gltf_preload_resolve_accessor_view(accessors,
                                                      accessor_count,
                                                      uv0_acc,
                                                      buffers,
                                                      buffer_count,
                                                      views,
                                                      view_count,
                                                      &out->uv0_view) &&
                   gltf_accessor_has_payload(&out->uv0_view) &&
                   gltf_accessor_is_texcoord(&out->uv0_view) &&
                   out->uv0_view.count >= out->pos_view.count;
    out->has_uv1 = gltf_preload_resolve_accessor_view(accessors,
                                                      accessor_count,
                                                      uv1_acc,
                                                      buffers,
                                                      buffer_count,
                                                      views,
                                                      view_count,
                                                      &out->uv1_view) &&
                   gltf_accessor_has_payload(&out->uv1_view) &&
                   gltf_accessor_is_texcoord(&out->uv1_view) &&
                   out->uv1_view.count >= out->pos_view.count;
    out->has_colors = gltf_preload_resolve_accessor_view(accessors,
                                                         accessor_count,
                                                         color_acc,
                                                         buffers,
                                                         buffer_count,
                                                         views,
                                                         view_count,
                                                         &out->color_view) &&
                      gltf_accessor_has_payload(&out->color_view) &&
                      gltf_accessor_is_color(&out->color_view) &&
                      out->color_view.count >= out->pos_view.count;
    out->has_tangents = gltf_preload_resolve_accessor_view(accessors,
                                                           accessor_count,
                                                           tangent_acc,
                                                           buffers,
                                                           buffer_count,
                                                           views,
                                                           view_count,
                                                           &out->tangent_view) &&
                        gltf_accessor_has_payload(&out->tangent_view) &&
                        gltf_accessor_is_tangent(&out->tangent_view, 4) &&
                        out->tangent_view.count >= out->pos_view.count;
    out->has_joints = gltf_preload_resolve_accessor_view(accessors,
                                                         accessor_count,
                                                         joints_acc,
                                                         buffers,
                                                         buffer_count,
                                                         views,
                                                         view_count,
                                                         &out->joints_view) &&
                      gltf_accessor_has_payload(&out->joints_view) &&
                      gltf_accessor_is_joints(&out->joints_view) &&
                      out->joints_view.count >= out->pos_view.count;
    out->has_weights = gltf_preload_resolve_accessor_view(accessors,
                                                          accessor_count,
                                                          weights_acc,
                                                          buffers,
                                                          buffer_count,
                                                          views,
                                                          view_count,
                                                          &out->weights_view) &&
                       gltf_accessor_has_payload(&out->weights_view) &&
                       gltf_accessor_is_weights(&out->weights_view) &&
                       out->weights_view.count >= out->pos_view.count;
    out->has_joints1 = gltf_preload_resolve_accessor_view(accessors,
                                                          accessor_count,
                                                          joints1_acc,
                                                          buffers,
                                                          buffer_count,
                                                          views,
                                                          view_count,
                                                          &out->joints1_view) &&
                       gltf_accessor_has_payload(&out->joints1_view) &&
                       gltf_accessor_is_joints(&out->joints1_view) &&
                       out->joints1_view.count >= out->pos_view.count;
    out->has_weights1 = gltf_preload_resolve_accessor_view(accessors,
                                                           accessor_count,
                                                           weights1_acc,
                                                           buffers,
                                                           buffer_count,
                                                           views,
                                                           view_count,
                                                           &out->weights1_view) &&
                        gltf_accessor_has_payload(&out->weights1_view) &&
                        gltf_accessor_is_weights(&out->weights1_view) &&
                        out->weights1_view.count >= out->pos_view.count;
    if ((out->has_joints != out->has_weights) || (out->has_joints1 != out->has_weights1))
        return 0;
    out->has_skinning_attrs =
        (out->has_joints && out->has_weights) || (out->has_joints1 && out->has_weights1);
    out->has_indices = gltf_preload_resolve_accessor_view(accessors,
                                                          accessor_count,
                                                          idx_acc,
                                                          buffers,
                                                          buffer_count,
                                                          views,
                                                          view_count,
                                                          &out->idx_view) &&
                       out->idx_view.comp_count == 1 &&
                       (out->idx_view.comp_type == 5121 || out->idx_view.comp_type == 5123 ||
                        out->idx_view.comp_type == 5125);
    out->vertex_count_i = out->pos_view.count;
    out->source_index_count = out->has_indices ? out->idx_view.count : out->vertex_count_i;
    if (out->vertex_count_i < 3 || out->source_index_count < 3)
        return 0;
    if (!gltf_preload_topology_index_capacity(
            out->mode, out->source_index_count, &out->index_capacity_count) ||
        out->index_capacity_count == 0)
        return 0;
    return 1;
}

/// @brief Decode each vertex's attributes into @p vertices, normalizing skin influences.
/// @details Skin influences that cannot be represented (a fifth meaningful influence, or a
///          joint index past VGFX3D_MAX_BONES) are counted into @p skin_stats_io so the
///          committing thread can surface load warnings — the decode itself runs on a
///          preload worker where the thread-local warning store is not the loader's.
/// @param[in] a Resolved primitive accessor state.
/// @param[out] vertices Zero-initialized packed vertex array with one entry per position.
/// @param[out] out_bone_count Receives one past the highest retained joint index.
/// @param[out] out_valid_normals Optional flag reporting whether all authored normals were valid.
/// @param[out] out_valid_tangents Optional flag reporting whether all authored tangents were valid.
/// @param[in,out] skin_stats_io Optional counters updated for truncated or out-of-range influences.
/// @return Non-zero on success; zero to skip the primitive when required source data is
///         non-finite or the arguments are invalid.
static int gltf_preload_build_primitive_vertices(const gltf_primitive_attribs_t *a,
                                                 vgfx3d_vertex_t *vertices,
                                                 uint32_t *out_bone_count,
                                                 int *out_valid_normals,
                                                 int *out_valid_tangents,
                                                 gltf_skin_import_stats_t *skin_stats_io) {
    uint32_t bone_count = 0u;
    int valid_normals = a && a->has_normals;
    int valid_tangents = a && a->has_tangents;
    if (out_valid_normals)
        *out_valid_normals = 0;
    if (out_valid_tangents)
        *out_valid_tangents = 0;
    if (!a || !vertices || !out_bone_count)
        return 0;
    for (int32_t vi = 0; vi < a->vertex_count_i; vi++) {
        float pos[3] = {0.0f, 0.0f, 0.0f};
        float nrm[3] = {0.0f, 0.0f, 0.0f};
        float uv[2] = {0.0f, 0.0f};
        float uv1[2] = {0.0f, 0.0f};
        float color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        float tangent[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        float weights[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        uint32_t joints[4] = {0u, 0u, 0u, 0u};
        float weights1[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        uint32_t joints1[4] = {0u, 0u, 0u, 0u};
        gltf_accessor_read_f32(&a->pos_view, vi, pos, 3);
        if (a->has_normals)
            gltf_accessor_read_f32(&a->norm_view, vi, nrm, 3);
        if (!gltf_preload_floats_are_finite(pos, 3) ||
            (a->has_normals && !gltf_preload_floats_are_finite(nrm, 3)))
            return 0;
        if (a->has_normals && !gltf_normalize_vec3f_in_place(nrm)) {
            valid_normals = 0;
            nrm[0] = 0.0f;
            nrm[1] = 0.0f;
            nrm[2] = 0.0f;
        }
        if (a->has_uv0) {
            gltf_accessor_read_f32(&a->uv0_view, vi, uv, 2);
            if (!gltf_preload_floats_are_finite(uv, 2))
                return 0;
        }
        if (a->has_uv1) {
            gltf_accessor_read_f32(&a->uv1_view, vi, uv1, 2);
            if (!gltf_preload_floats_are_finite(uv1, 2))
                return 0;
        } else {
            uv1[0] = uv[0];
            uv1[1] = uv[1];
        }
        if (a->has_colors) {
            gltf_accessor_read_f32(&a->color_view, vi, color, 4);
            if (a->color_view.comp_count < 4)
                color[3] = 1.0f;
            if (!gltf_preload_floats_are_finite(color, 4))
                return 0;
        }
        if (a->has_tangents) {
            gltf_accessor_read_f32(&a->tangent_view, vi, tangent, 4);
            if (a->tangent_view.comp_count < 4)
                tangent[3] = 1.0f;
            if (!gltf_preload_floats_are_finite(tangent, 4))
                return 0;
            if (!gltf_sanitize_tangent4(tangent)) {
                valid_tangents = 0;
                tangent[0] = 1.0f;
                tangent[1] = 0.0f;
                tangent[2] = 0.0f;
                tangent[3] = 1.0f;
            }
        }
        memcpy(vertices[vi].pos, pos, sizeof(vertices[vi].pos));
        memcpy(vertices[vi].normal, nrm, sizeof(vertices[vi].normal));
        memcpy(vertices[vi].uv, uv, sizeof(vertices[vi].uv));
        memcpy(vertices[vi].uv1, uv1, sizeof(vertices[vi].uv1));
        memcpy(vertices[vi].color, color, sizeof(vertices[vi].color));
        memcpy(vertices[vi].tangent, tangent, sizeof(vertices[vi].tangent));
        if (a->has_skinning_attrs) {
            uint32_t vertex_bone_count;
            if (a->has_joints)
                gltf_accessor_read_u32(&a->joints_view, vi, joints, 4);
            if (a->has_weights)
                gltf_accessor_read_f32(&a->weights_view, vi, weights, 4);
            if (a->has_joints1)
                gltf_accessor_read_u32(&a->joints1_view, vi, joints1, 4);
            if (a->has_weights1)
                gltf_accessor_read_f32(&a->weights1_view, vi, weights1, 4);
            int truncated = 0;
            int out_of_range = 0;
            if (a->has_joints1 && a->has_weights1) {
                uint32_t merged_joints[4] = {0u, 0u, 0u, 0u};
                float merged_weights[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                if (a->has_joints && a->has_weights) {
                    for (int j = 0; j < 4; j++)
                        gltf_add_top_joint_influence(joints[j],
                                                     weights[j],
                                                     merged_joints,
                                                     merged_weights,
                                                     &truncated,
                                                     &out_of_range);
                }
                for (int j = 0; j < 4; j++)
                    gltf_add_top_joint_influence(joints1[j],
                                                 weights1[j],
                                                 merged_joints,
                                                 merged_weights,
                                                 &truncated,
                                                 &out_of_range);
                gltf_normalize_joint_influences(merged_weights);
                memcpy(joints, merged_joints, sizeof(joints));
                memcpy(weights, merged_weights, sizeof(weights));
            } else {
                for (int j = 0; j < 4; j++) {
                    if (joints[j] >= VGFX3D_MAX_BONES) {
                        if (isfinite(weights[j]) && weights[j] > 1e-8f)
                            out_of_range = 1;
                        weights[j] = 0.0f;
                    }
                }
            }
            if (skin_stats_io) {
                if (truncated)
                    skin_stats_io->truncated_vertices++;
                if (out_of_range)
                    skin_stats_io->out_of_range_vertices++;
            }
            vertex_bone_count =
                gltf_preload_write_vertex_bone_weights(&vertices[vi], joints, weights);
            if (vertex_bone_count > bone_count)
                bone_count = vertex_bone_count;
        }
    }
    *out_bone_count = bone_count;
    if (out_valid_normals)
        *out_valid_normals = valid_normals;
    if (out_valid_tangents)
        *out_valid_tangents = valid_tangents;
    return 1;
}

/// @brief Triangulate the primitive's topology (modes 4/5/6) into @p indices.
/// @param[in] a Resolved primitive topology and optional index accessor.
/// @param[in] vertices Packed vertices used to reject geometrically degenerate triangles.
/// @param[out] indices Triangle-list output array sized to the resolved index capacity.
/// @param[out] out_index_count Receives the number of emitted indices.
static void gltf_preload_triangulate_primitive(const gltf_primitive_attribs_t *a,
                                               vgfx3d_vertex_t *vertices,
                                               uint32_t *indices,
                                               uint32_t *out_index_count) {
    const gltf_accessor_view_t *idx = a->has_indices ? &a->idx_view : NULL;
    uint32_t index_count = 0;
    if (a->mode == 4) {
        for (int32_t ii = 0; ii + 2 < a->source_index_count; ii += 3) {
            gltf_preload_emit_pod_triangle(vertices,
                                           (uint32_t)a->vertex_count_i,
                                           gltf_preload_read_index_or_vertex(idx, ii),
                                           gltf_preload_read_index_or_vertex(idx, ii + 1),
                                           gltf_preload_read_index_or_vertex(idx, ii + 2),
                                           indices,
                                           &index_count);
        }
    } else if (a->mode == 5) {
        for (int32_t ii = 0; ii + 2 < a->source_index_count; ii++) {
            uint32_t i0 = gltf_preload_read_index_or_vertex(idx, ii);
            uint32_t i1 = gltf_preload_read_index_or_vertex(idx, ii + 1);
            uint32_t i2 = gltf_preload_read_index_or_vertex(idx, ii + 2);
            if ((ii & 1) != 0) {
                uint32_t tmp = i0;
                i0 = i1;
                i1 = tmp;
            }
            gltf_preload_emit_pod_triangle(
                vertices, (uint32_t)a->vertex_count_i, i0, i1, i2, indices, &index_count);
        }
    } else {
        uint32_t base = gltf_preload_read_index_or_vertex(idx, 0);
        for (int32_t ii = 1; ii + 1 < a->source_index_count; ii++) {
            gltf_preload_emit_pod_triangle(vertices,
                                           (uint32_t)a->vertex_count_i,
                                           base,
                                           gltf_preload_read_index_or_vertex(idx, ii),
                                           gltf_preload_read_index_or_vertex(idx, ii + 1),
                                           indices,
                                           &index_count);
        }
    }
    *out_index_count = index_count;
}

/// @brief Stage one mesh primitive during the preload pass: resolve its vertex attributes
///   and indices from the glTF JSON and buffer views, then record the built geometry into
///   @p bundle keyed by (@p mesh_index, @p primitive_index) for later mesh assembly. Writes a
///   message into @p error (capacity @p error_cap) on a fatal failure.
/// @param[in,out] bundle Bundle receiving packed mesh and morph dependencies.
/// @param[in] json Complete glTF JSON text.
/// @param json_len Length of @p json in bytes.
/// @param mesh_index Zero-based source mesh index.
/// @param primitive_index Zero-based primitive index within the source mesh.
/// @param mesh_start Inclusive byte offset of the parent mesh object.
/// @param mesh_end Exclusive byte offset bounding the parent mesh object.
/// @param prim_start Inclusive byte offset of the primitive object.
/// @param prim_end Exclusive byte offset bounding the primitive object.
/// @param[in] buffers Staged buffer-reference table.
/// @param buffer_count Number of entries in @p buffers.
/// @param[in] views Parsed buffer-view-reference table.
/// @param view_count Number of entries in @p views.
/// @param[in] accessors Parsed accessor-reference table.
/// @param accessor_count Number of entries in @p accessors.
/// @param[out] error Optional caller-provided error buffer.
/// @param error_cap Capacity of @p error in bytes.
/// @return Non-zero to continue staging (primitive staged or harmlessly skipped); 0 on a
///   fatal error such as an allocation failure.
static int gltf_preload_stage_mesh_primitive(rt_gltf_preload_bundle *bundle,
                                             const char *json,
                                             size_t json_len,
                                             int mesh_index,
                                             int primitive_index,
                                             size_t mesh_start,
                                             size_t mesh_end,
                                             size_t prim_start,
                                             size_t prim_end,
                                             const gltf_preload_buffer_ref_t *buffers,
                                             int buffer_count,
                                             const gltf_preload_buffer_view_ref_t *views,
                                             int view_count,
                                             const gltf_preload_accessor_ref_t *accessors,
                                             int accessor_count,
                                             char *error,
                                             size_t error_cap) {
    gltf_primitive_attribs_t attribs;
    uint32_t flags = 0u;
    uint32_t bone_count = 0u;
    int valid_normals = 0;
    int valid_tangents = 0;
    vgfx3d_vertex_t *vertices = NULL;
    uint32_t *indices = NULL;
    uint32_t index_count = 0;
    size_t vertex_bytes;
    size_t index_bytes;
    gltf_skin_import_stats_t skin_stats = {0u, 0u};
    int ok = 1;

    if (!bundle || !json || !accessors)
        return 1;
    if (!gltf_preload_resolve_primitive_attribs(json,
                                                json_len,
                                                prim_start,
                                                prim_end,
                                                buffers,
                                                buffer_count,
                                                views,
                                                view_count,
                                                accessors,
                                                accessor_count,
                                                &attribs))
        return 1;
    if (!gltf_checked_mul_size(
            (size_t)attribs.vertex_count_i, sizeof(vgfx3d_vertex_t), &vertex_bytes) ||
        !gltf_checked_mul_size(
            (size_t)attribs.index_capacity_count, sizeof(uint32_t), &index_bytes))
        return 1;
    if (vertex_bytes == 0 || index_bytes == 0)
        return 1;

    vertices = (vgfx3d_vertex_t *)calloc((size_t)attribs.vertex_count_i, sizeof(*vertices));
    indices = (uint32_t *)malloc(index_bytes);
    if (!vertices || !indices) {
        free(vertices);
        free(indices);
        gltf_preload_set_error(error, error_cap, "failed to stage glTF dependency");
        return 0;
    }

    if (!gltf_preload_build_primitive_vertices(
            &attribs, vertices, &bone_count, &valid_normals, &valid_tangents, &skin_stats))
        goto done;

    gltf_preload_triangulate_primitive(&attribs, vertices, indices, &index_count);
    if (index_count == 0)
        goto done;
    if (valid_normals)
        flags |= GLTF_PRELOAD_MESH_POD_HAS_NORMALS;
    if (attribs.has_uv0)
        flags |= GLTF_PRELOAD_MESH_POD_HAS_UV0;
    if (attribs.has_uv1)
        flags |= GLTF_PRELOAD_MESH_POD_HAS_UV1;
    if (valid_tangents)
        flags |= GLTF_PRELOAD_MESH_POD_HAS_TANGENTS;
    if (attribs.has_skinning_attrs)
        flags |= GLTF_PRELOAD_MESH_POD_HAS_SKINNING;
    ok = gltf_preload_pack_mesh_pod(bundle,
                                    mesh_index,
                                    primitive_index,
                                    flags,
                                    vertices,
                                    (uint32_t)attribs.vertex_count_i,
                                    indices,
                                    index_count,
                                    bone_count,
                                    &skin_stats,
                                    error,
                                    error_cap);
    if (ok) {
        ok = gltf_preload_stage_morph_targets(bundle,
                                              json,
                                              json_len,
                                              mesh_index,
                                              primitive_index,
                                              mesh_start,
                                              mesh_end,
                                              prim_start,
                                              prim_end,
                                              buffers,
                                              buffer_count,
                                              views,
                                              view_count,
                                              accessors,
                                              accessor_count,
                                              attribs.vertex_count_i,
                                              error,
                                              error_cap);
    }

done:
    free(vertices);
    free(indices);
    return ok;
}

/// @brief Stage every mesh primitive as a packed POD blob (and its morph targets) off the main
/// thread.
/// @details Walks "meshes"[].primitives[], resolves each attribute/index accessor, triangulates the
///          topology, normalizes skinning, and emits "VGMP"/"VGMT" dependencies so the main-thread
///          load can build meshes without re-parsing. Sets @p error and returns 0 on a staging
///          failure.
/// @param[in,out] bundle Bundle receiving packed primitive dependencies.
/// @param[in] json Complete glTF JSON text.
/// @param json_len Length of @p json in bytes.
/// @param[in] buffers Staged buffer-reference table.
/// @param buffer_count Number of entries in @p buffers.
/// @param[in] views Parsed buffer-view-reference table.
/// @param view_count Number of entries in @p views.
/// @param[out] error Optional caller-provided error buffer.
/// @param error_cap Capacity of @p error in bytes.
/// @return Non-zero when all primitives were staged or harmlessly skipped; zero on
///         accessor-table, allocation, or dependency-staging failure.
static int gltf_preload_stage_meshes(rt_gltf_preload_bundle *bundle,
                                     const char *json,
                                     size_t json_len,
                                     const gltf_preload_buffer_ref_t *buffers,
                                     int buffer_count,
                                     const gltf_preload_buffer_view_ref_t *views,
                                     int view_count,
                                     char *error,
                                     size_t error_cap) {
    size_t array_start;
    size_t array_end;
    size_t pos;
    gltf_preload_accessor_ref_t *accessors = NULL;
    int accessor_count = 0;
    int mesh_index = 0;
    if (!gltf_json_find_top_level_array(json, json_len, "meshes", &array_start, &array_end))
        return 1;
    if (!gltf_preload_parse_accessors(
            json, json_len, &accessors, &accessor_count, error, error_cap))
        return 0;
    pos = array_start + 1u;
    while (pos < array_end) {
        pos = gltf_json_skip_ws(json, json_len, pos);
        if (pos >= array_end || json[pos] == ']')
            break;
        if (json[pos] == '{') {
            size_t object_end = gltf_json_find_matching(json, json_len, pos, '{', '}');
            size_t prims_start;
            size_t prims_end;
            int primitive_index = 0;
            if (object_end == SIZE_MAX || object_end > array_end)
                break;
            if (gltf_json_object_find_value(
                    json, json_len, pos, object_end, "primitives", &prims_start, &prims_end) &&
                prims_start < prims_end && json[prims_start] == '[') {
                size_t prim_pos = prims_start + 1u;
                while (prim_pos < prims_end) {
                    prim_pos = gltf_json_skip_ws(json, json_len, prim_pos);
                    if (prim_pos >= prims_end || json[prim_pos] == ']')
                        break;
                    if (json[prim_pos] == '{') {
                        size_t prim_end =
                            gltf_json_find_matching(json, json_len, prim_pos, '{', '}');
                        if (prim_end == SIZE_MAX || prim_end > prims_end)
                            break;
                        if (!gltf_preload_stage_mesh_primitive(bundle,
                                                               json,
                                                               json_len,
                                                               mesh_index,
                                                               primitive_index,
                                                               pos,
                                                               object_end,
                                                               prim_pos,
                                                               prim_end,
                                                               buffers,
                                                               buffer_count,
                                                               views,
                                                               view_count,
                                                               accessors,
                                                               accessor_count,
                                                               error,
                                                               error_cap)) {
                            free(accessors);
                            return 0;
                        }
                        primitive_index++;
                        prim_pos = prim_end;
                    } else {
                        prim_pos = gltf_json_skip_value(json, json_len, prim_pos);
                        if (prim_pos == SIZE_MAX)
                            break;
                    }
                    prim_pos = gltf_json_skip_ws(json, json_len, prim_pos);
                    if (prim_pos < prims_end && json[prim_pos] == ',')
                        prim_pos++;
                }
            }
            mesh_index++;
            pos = object_end;
        } else {
            pos = gltf_json_skip_value(json, json_len, pos);
            if (pos == SIZE_MAX)
                break;
        }
        pos = gltf_json_skip_ws(json, json_len, pos);
        if (pos < array_end && json[pos] == ',')
            pos++;
    }
    free(accessors);
    return 1;
}

/// @brief Stage an inline image's bytes under its image-index key (decoding to RGBA when possible).
/// @param[in,out] bundle Bundle receiving the owned image dependency.
/// @param image_index Zero-based source image index used in the dependency key.
/// @param[in] mime_type Optional declared image MIME type.
/// @param[in,out] data Owned image bytes transferred to the bundle or released.
/// @param data_len Number of bytes in @p data.
/// @param required Non-zero when empty or undecodable supported data is fatal.
/// @param[out] error Optional caller-provided error buffer.
/// @param error_cap Capacity of @p error in bytes.
/// @return Non-zero when the image was staged or may be omitted; zero on required-image
///         or dependency-staging failure.
static int gltf_preload_stage_image_bytes(rt_gltf_preload_bundle *bundle,
                                          int image_index,
                                          const char *mime_type,
                                          uint8_t *data,
                                          size_t data_len,
                                          int required,
                                          char *error,
                                          size_t error_cap) {
    char key[96];
    if (!data || data_len == 0) {
        free(data);
        if (required) {
            gltf_preload_set_error(error, error_cap, "invalid glTF image payload");
            return 0;
        }
        return 1;
    }
    gltf_preload_image_key(image_index, mime_type, key, sizeof(key));
    return gltf_preload_bundle_add_image_payload(
        bundle, key, mime_type ? mime_type : key, data, data_len, required, error, error_cap);
}

/// @brief Mark which image sources are actually referenced by a texture, in the @p required bitmap.
/// @details Scans the top-level "textures" array and sets required[source] for each texture's image
///          index, so only images a material can use are treated as mandatory to stage.
/// @param[in] json Complete glTF JSON text.
/// @param json_len Length of @p json in bytes.
/// @param[in,out] required Image-index bitmap updated for referenced sources.
/// @param required_count Number of entries in @p required.
static void gltf_preload_mark_required_images(const char *json,
                                              size_t json_len,
                                              uint8_t *required,
                                              int required_count) {
    size_t array_start;
    size_t array_end;
    size_t pos;
    if (!json || !required || required_count <= 0)
        return;
    if (!gltf_json_find_top_level_array(json, json_len, "textures", &array_start, &array_end))
        return;
    pos = array_start + 1u;
    while (pos < array_end) {
        pos = gltf_json_skip_ws(json, json_len, pos);
        if (pos >= array_end || json[pos] == ']')
            break;
        if (json[pos] == '{') {
            size_t object_end = gltf_json_find_matching(json, json_len, pos, '{', '}');
            int source;
            if (object_end == SIZE_MAX || object_end > array_end)
                break;
            source = gltf_json_texture_source_index(json, json_len, pos, object_end);
            if (source >= 0 && source < required_count)
                required[source] = 1u;
            pos = object_end;
        } else {
            pos = gltf_json_skip_value(json, json_len, pos);
            if (pos == SIZE_MAX)
                break;
        }
        pos = gltf_json_skip_ws(json, json_len, pos);
        if (pos < array_end && json[pos] == ',')
            pos++;
    }
}

/// @brief Count object-valued elements within one bounded JSON array.
/// @param[in] json Complete JSON text.
/// @param json_len Length of @p json in bytes.
/// @param array_start Inclusive byte offset of the array's opening bracket.
/// @param array_end Exclusive byte offset bounding the array.
/// @return Number of well-formed object entries encountered before the array ends or
///         malformed syntax prevents further scanning.
static int gltf_json_count_objects_in_array(const char *json,
                                            size_t json_len,
                                            size_t array_start,
                                            size_t array_end) {
    size_t pos = array_start + 1u;
    int count = 0;
    while (pos < array_end) {
        pos = gltf_json_skip_ws(json, json_len, pos);
        if (pos >= array_end || json[pos] == ']')
            break;
        if (json[pos] == '{') {
            size_t object_end = gltf_json_find_matching(json, json_len, pos, '{', '}');
            if (object_end == SIZE_MAX || object_end > array_end)
                break;
            count++;
            pos = object_end;
        } else {
            pos = gltf_json_skip_value(json, json_len, pos);
            if (pos == SIZE_MAX)
                break;
        }
        pos = gltf_json_skip_ws(json, json_len, pos);
        if (pos < array_end && json[pos] == ',')
            pos++;
    }
    return count;
}

/// @brief Allocate and populate the texture-referenced image bitmap for an images array.
/// @param[in] json Complete glTF JSON text.
/// @param json_len Length of @p json in bytes.
/// @param array_start Inclusive byte offset of the `images` array.
/// @param array_end Exclusive byte offset bounding the `images` array.
/// @param[out] out_image_count Optional destination for the counted image objects.
/// @param[out] out_required Optional destination for the owned required-image bitmap.
/// @param[out] error Optional caller-provided error buffer.
/// @param error_cap Capacity of @p error in bytes.
/// @return Non-zero when the bitmap is prepared or no image objects exist; zero on
///         untrusted-count rejection or allocation failure.
static int gltf_preload_prepare_required_images(const char *json,
                                                size_t json_len,
                                                size_t array_start,
                                                size_t array_end,
                                                int *out_image_count,
                                                uint8_t **out_required,
                                                char *error,
                                                size_t error_cap) {
    int image_count = gltf_json_count_objects_in_array(json, json_len, array_start, array_end);
    uint8_t *required = NULL;
    if (out_image_count)
        *out_image_count = image_count;
    if (out_required)
        *out_required = NULL;
    if (image_count <= 0)
        return 1;
    if (!rt_untrusted_count_ok((int64_t)image_count, 1u, json_len)) {
        gltf_preload_set_error(error, error_cap, "failed to stage glTF preload");
        return 0;
    }
    required = (uint8_t *)calloc((size_t)image_count, sizeof(uint8_t));
    if (!required) {
        gltf_preload_set_error(error, error_cap, "failed to stage glTF preload");
        return 0;
    }
    gltf_preload_mark_required_images(json, json_len, required, image_count);
    if (out_required)
        *out_required = required;
    else
        free(required);
    return 1;
}

/// @brief Verify supported data-URI payloads for every texture-referenced image.
/// @details Marks required images (via gltf_preload_mark_required_images), then decodes each
///          referenced inline KTX2 or supported raster image to reject corrupt payloads early.
/// @param[in] json Complete glTF JSON text.
/// @param json_len Length of @p json in bytes.
/// @return Non-zero when every supported required inline payload is valid; zero when
///         allocation fails or a supported payload is corrupt.
static int gltf_validate_required_data_uri_images(const char *json, size_t json_len) {
    size_t array_start;
    size_t array_end;
    size_t pos;
    int image_count;
    int index = 0;
    uint8_t *required = NULL;
    if (!gltf_json_find_top_level_array(json, json_len, "images", &array_start, &array_end))
        return 1;

    image_count = gltf_json_count_objects_in_array(json, json_len, array_start, array_end);
    if (image_count <= 0)
        return 1;
    if (!rt_untrusted_count_ok((int64_t)image_count, 1u, json_len))
        return 0;

    required = (uint8_t *)calloc((size_t)image_count, sizeof(uint8_t));
    if (!required)
        return 0;
    gltf_preload_mark_required_images(json, json_len, required, image_count);

    pos = array_start + 1u;
    while (pos < array_end) {
        pos = gltf_json_skip_ws(json, json_len, pos);
        if (pos >= array_end || json[pos] == ']')
            break;
        if (json[pos] == '{') {
            size_t object_end = gltf_json_find_matching(json, json_len, pos, '{', '}');
            char *uri;
            char *mime_type;
            int required_image;
            if (object_end == SIZE_MAX || object_end > array_end)
                break;
            uri = gltf_json_object_get_string(json, json_len, pos, object_end, "uri");
            mime_type = gltf_json_object_get_string(json, json_len, pos, object_end, "mimeType");
            required_image = index < image_count && required[index] != 0;
            if (required_image && uri && strncmp(uri, "data:", 5) == 0) {
                char parsed_mime[64];
                const char *image_type;
                uint8_t *data = NULL;
                uint8_t *rgba_blob = NULL;
                size_t data_len = 0;
                size_t rgba_len = 0;
                int ok = 1;
                gltf_data_uri_copy_mime(uri, parsed_mime, sizeof(parsed_mime));
                image_type = mime_type ? mime_type : parsed_mime;
                if (gltf_image_is_ktx2(image_type)) {
                    void *ktx_asset = NULL;
                    ok = gltf_parse_data_uri(
                        uri, parsed_mime, sizeof(parsed_mime), &data, &data_len);
                    if (ok)
                        ktx_asset = gltf_decode_ktx2_payload(data, data_len);
                    ok = ktx_asset != NULL;
                    if (ktx_asset && rt_obj_release_check0(ktx_asset))
                        rt_obj_free(ktx_asset);
                    free(data);
                } else if (gltf_preload_image_is_supported_format(image_type)) {
                    ok = gltf_parse_data_uri(
                             uri, parsed_mime, sizeof(parsed_mime), &data, &data_len) &&
                         gltf_decode_image_payload_to_rgba_blob(
                             image_type, data, data_len, &rgba_blob, &rgba_len);
                    free(data);
                    free(rgba_blob);
                }
                if (!ok) {
                    free(uri);
                    free(mime_type);
                    free(required);
                    return 0;
                }
            }
            free(uri);
            free(mime_type);
            index++;
            pos = object_end;
        } else {
            pos = gltf_json_skip_value(json, json_len, pos);
            if (pos == SIZE_MAX)
                break;
        }
        pos = gltf_json_skip_ws(json, json_len, pos);
        if (pos < array_end && json[pos] == ',')
            pos++;
    }
    free(required);
    return 1;
}

/// @brief Decode and stage one image stored in a data URI.
/// @param[in,out] bundle Bundle receiving the owned image dependency.
/// @param image_index Zero-based source image index.
/// @param[in] mime_type Optional MIME type declared by the image object.
/// @param[in] uri Image data URI.
/// @param required_image Non-zero when a malformed supported image is fatal.
/// @param[out] error Optional caller-provided error buffer.
/// @param error_cap Capacity of @p error in bytes.
/// @return Non-zero when the image was staged or may be skipped; zero when required
///         image data is invalid or staging fails.
static int gltf_preload_stage_data_uri_image(rt_gltf_preload_bundle *bundle,
                                             int image_index,
                                             const char *mime_type,
                                             const char *uri,
                                             int required_image,
                                             char *error,
                                             size_t error_cap) {
    char parsed_mime[64];
    uint8_t *data = NULL;
    size_t data_len = 0;
    parsed_mime[0] = '\0';
    if (gltf_parse_data_uri(uri, parsed_mime, sizeof(parsed_mime), &data, &data_len))
        return gltf_preload_stage_image_bytes(bundle,
                                              image_index,
                                              mime_type ? mime_type : parsed_mime,
                                              data,
                                              data_len,
                                              required_image,
                                              error,
                                              error_cap);
    if (required_image && gltf_preload_image_is_supported_format(mime_type)) {
        gltf_preload_set_error(error, error_cap, "invalid glTF image payload");
        return 0;
    }
    return 1;
}

/// @brief Dispatch an image URI to inline-data or external-file staging.
/// @param[in,out] bundle Bundle receiving the image dependency.
/// @param[in] model_path Root asset path used for external URI resolution.
/// @param image_index Zero-based source image index.
/// @param[in] mime_type Optional MIME type declared by the image object.
/// @param[in] uri Data or external image URI.
/// @param load_assets Non-zero to use asset-system loading for external images.
/// @param required_image Non-zero when failure to stage a supported image is fatal.
/// @param[out] error Optional caller-provided error buffer.
/// @param error_cap Capacity of @p error in bytes.
/// @return Result of the selected image-staging path.
static int gltf_preload_stage_image_uri(rt_gltf_preload_bundle *bundle,
                                        const char *model_path,
                                        int image_index,
                                        const char *mime_type,
                                        const char *uri,
                                        int load_assets,
                                        int required_image,
                                        char *error,
                                        size_t error_cap) {
    if (strncmp(uri, "data:", 5) == 0)
        return gltf_preload_stage_data_uri_image(
            bundle, image_index, mime_type, uri, required_image, error, error_cap);
    return gltf_preload_stage_external_image(
        bundle, model_path, uri, load_assets, required_image, error, error_cap);
}

/// @brief Copy and stage an image embedded in a glTF buffer view.
/// @param[in,out] bundle Bundle receiving the owned image dependency.
/// @param[in] json Complete glTF JSON text.
/// @param json_len Length of @p json in bytes.
/// @param image_index Zero-based source image index.
/// @param object_start Inclusive byte offset of the image object.
/// @param object_end Exclusive byte offset bounding the image object.
/// @param[in] mime_type Optional declared image MIME type.
/// @param required_image Non-zero when an absent, empty, or invalid view is fatal.
/// @param[in] buffers Staged buffer-reference table.
/// @param buffer_count Number of entries in @p buffers.
/// @param[in] views Parsed buffer-view-reference table.
/// @param view_count Number of entries in @p views.
/// @param[out] error Optional caller-provided error buffer.
/// @param error_cap Capacity of @p error in bytes.
/// @return Non-zero when the image was staged or may be omitted; zero on required-image,
///         allocation, or dependency-staging failure.
static int gltf_preload_stage_buffer_view_image(rt_gltf_preload_bundle *bundle,
                                                const char *json,
                                                size_t json_len,
                                                int image_index,
                                                size_t object_start,
                                                size_t object_end,
                                                const char *mime_type,
                                                int required_image,
                                                const gltf_preload_buffer_ref_t *buffers,
                                                int buffer_count,
                                                const gltf_preload_buffer_view_ref_t *views,
                                                int view_count,
                                                char *error,
                                                size_t error_cap) {
    int view_index =
        gltf_json_object_get_int(json, json_len, object_start, object_end, "bufferView", -1);
    int staged = 0;
    if (view_index >= 0 && view_index < view_count && views && views[view_index].valid) {
        const gltf_preload_buffer_view_ref_t *view = &views[view_index];
        if (view->buffer >= 0 && view->buffer < buffer_count && buffers &&
            buffers[view->buffer].data) {
            size_t end;
            if (view->byte_length == 0 && required_image) {
                gltf_preload_set_error(error, error_cap, "invalid glTF image payload");
                return 0;
            }
            if (view->byte_length > 0 &&
                gltf_checked_add_size(view->byte_offset, view->byte_length, &end) &&
                end <= buffers[view->buffer].len) {
                uint8_t *copy = (uint8_t *)malloc(view->byte_length);
                if (!copy) {
                    gltf_preload_set_error(error, error_cap, "failed to stage glTF dependency");
                    return 0;
                }
                memcpy(copy, buffers[view->buffer].data + view->byte_offset, view->byte_length);
                staged = 1;
                if (!gltf_preload_stage_image_bytes(bundle,
                                                    image_index,
                                                    mime_type,
                                                    copy,
                                                    view->byte_length,
                                                    required_image,
                                                    error,
                                                    error_cap))
                    return 0;
            }
        }
    }
    if (!staged && required_image) {
        gltf_preload_set_error(error, error_cap, "failed to stage glTF dependency");
        return 0;
    }
    return 1;
}

/// @brief Stage one parsed image object from its URI or buffer-view representation.
/// @param[in,out] bundle Bundle receiving the image dependency.
/// @param[in] model_path Root asset path used for relative URI resolution.
/// @param[in] json Complete glTF JSON text.
/// @param json_len Length of @p json in bytes.
/// @param image_index Zero-based source image index.
/// @param object_start Inclusive byte offset of the image object.
/// @param object_end Exclusive byte offset bounding the image object.
/// @param load_assets Non-zero to use asset-system loading for external images.
/// @param required_image Non-zero when staging failure is fatal.
/// @param[in] buffers Staged buffer-reference table.
/// @param buffer_count Number of entries in @p buffers.
/// @param[in] views Parsed buffer-view-reference table.
/// @param view_count Number of entries in @p views.
/// @param[out] error Optional caller-provided error buffer.
/// @param error_cap Capacity of @p error in bytes.
/// @return Result of URI or buffer-view image staging.
static int gltf_preload_stage_image_object(rt_gltf_preload_bundle *bundle,
                                           const char *model_path,
                                           const char *json,
                                           size_t json_len,
                                           int image_index,
                                           size_t object_start,
                                           size_t object_end,
                                           int load_assets,
                                           int required_image,
                                           const gltf_preload_buffer_ref_t *buffers,
                                           int buffer_count,
                                           const gltf_preload_buffer_view_ref_t *views,
                                           int view_count,
                                           char *error,
                                           size_t error_cap) {
    char *uri = gltf_json_object_get_string(json, json_len, object_start, object_end, "uri");
    char *mime_type =
        gltf_json_object_get_string(json, json_len, object_start, object_end, "mimeType");
    int ok;
    if (uri) {
        ok = gltf_preload_stage_image_uri(bundle,
                                          model_path,
                                          image_index,
                                          mime_type,
                                          uri,
                                          load_assets,
                                          required_image,
                                          error,
                                          error_cap);
    } else {
        ok = gltf_preload_stage_buffer_view_image(bundle,
                                                  json,
                                                  json_len,
                                                  image_index,
                                                  object_start,
                                                  object_end,
                                                  mime_type,
                                                  required_image,
                                                  buffers,
                                                  buffer_count,
                                                  views,
                                                  view_count,
                                                  error,
                                                  error_cap);
    }
    free(uri);
    free(mime_type);
    return ok;
}

/// @brief Stage every glTF image (inline data-URI, bufferView-embedded, or external file).
/// @details Walks the "images" array, marking texture-referenced images required, and stages each
///          image's bytes (decoding to RGBA when possible) under its image key. A required image
///          that cannot be staged sets @p error and fails; optional images are skipped on error.
/// @param[in,out] bundle Bundle receiving all staged image dependencies.
/// @param[in] model_path Root asset path used for relative URI resolution.
/// @param[in] json Complete glTF JSON text.
/// @param json_len Length of @p json in bytes.
/// @param load_assets Non-zero to use asset-system loading for external images.
/// @param[in] buffers Staged buffer-reference table.
/// @param buffer_count Number of entries in @p buffers.
/// @param[in] views Parsed buffer-view-reference table.
/// @param view_count Number of entries in @p views.
/// @param[out] error Optional caller-provided error buffer.
/// @param error_cap Capacity of @p error in bytes.
/// @return Non-zero when every required image was staged and optional failures were
///         skipped; zero on invalid image enumeration or staging failure.
static int gltf_preload_stage_images(rt_gltf_preload_bundle *bundle,
                                     const char *model_path,
                                     const char *json,
                                     size_t json_len,
                                     int load_assets,
                                     const gltf_preload_buffer_ref_t *buffers,
                                     int buffer_count,
                                     const gltf_preload_buffer_view_ref_t *views,
                                     int view_count,
                                     char *error,
                                     size_t error_cap) {
    size_t array_start;
    size_t array_end;
    size_t pos;
    int image_count;
    int index = 0;
    uint8_t *required = NULL;
    if (!gltf_json_find_top_level_array(json, json_len, "images", &array_start, &array_end))
        return 1;

    if (!gltf_preload_prepare_required_images(
            json, json_len, array_start, array_end, &image_count, &required, error, error_cap))
        return 0;

    pos = array_start + 1u;
    while (pos < array_end) {
        pos = gltf_json_skip_ws(json, json_len, pos);
        if (pos >= array_end || json[pos] == ']')
            break;
        if (json[pos] == '{') {
            size_t object_end = gltf_json_find_matching(json, json_len, pos, '{', '}');
            int required_image;
            if (object_end == SIZE_MAX || object_end > array_end)
                break;
            if (index >= image_count) {
                free(required);
                gltf_preload_set_error(error, error_cap, "invalid glTF image list");
                return 0;
            }
            required_image = required ? required[index] != 0 : 0;
            if (!gltf_preload_stage_image_object(bundle,
                                                 model_path,
                                                 json,
                                                 json_len,
                                                 index,
                                                 pos,
                                                 object_end,
                                                 load_assets,
                                                 required_image,
                                                 buffers,
                                                 buffer_count,
                                                 views,
                                                 view_count,
                                                 error,
                                                 error_cap)) {
                free(required);
                return 0;
            }
            index++;
            pos = object_end;
        } else {
            pos = gltf_json_skip_value(json, json_len, pos);
            if (pos == SIZE_MAX)
                break;
        }
        pos = gltf_json_skip_ws(json, json_len, pos);
        if (pos < array_end && json[pos] == ',')
            pos++;
    }
    free(required);
    return 1;
}

/// @brief Allocate a preload bundle and attach the caller-owned root asset bytes.
/// @param[in,out] root_data Root glTF or GLB allocation whose ownership transfers on success.
/// @param root_size Size of @p root_data in bytes.
/// @param[out] error Optional caller-provided error buffer.
/// @param error_cap Capacity of @p error in bytes.
/// @return Zero-initialized bundle owning @p root_data, or `NULL` on allocation failure.
static rt_gltf_preload_bundle *gltf_preload_bundle_alloc_with_root(uint8_t *root_data,
                                                                   size_t root_size,
                                                                   char *error,
                                                                   size_t error_cap) {
    rt_gltf_preload_bundle *bundle = (rt_gltf_preload_bundle *)calloc(1, sizeof(*bundle));
    if (!bundle) {
        gltf_preload_set_error(error, error_cap, "failed to stage glTF preload");
        return NULL;
    }
    bundle->root_data = root_data;
    bundle->root_size = root_size;
    return bundle;
}

/// @brief Run buffer, accessor, mesh, morph, and image staging for validated glTF JSON.
/// @details Temporary buffer and buffer-view reference tables are always released before
///          returning; successfully inserted dependencies remain owned by @p bundle.
/// @param[in,out] bundle Bundle receiving all staged dependencies.
/// @param[in] model_path Root asset path used for relative URI resolution.
/// @param[in] json Validated, mutable NUL-terminated glTF JSON text.
/// @param json_len Length of @p json excluding the terminator.
/// @param load_assets Non-zero to use asset-system loading for external resources.
/// @param[out] error Optional caller-provided error buffer.
/// @param error_cap Capacity of @p error in bytes.
/// @return Non-zero when every staging phase succeeds; zero on validation, allocation,
///         or dependency-staging failure.
static int gltf_preload_stage_bundle_json(rt_gltf_preload_bundle *bundle,
                                          const char *model_path,
                                          char *json,
                                          size_t json_len,
                                          int load_assets,
                                          char *error,
                                          size_t error_cap) {
    gltf_json_reset_lookup_cache();
    gltf_preload_buffer_ref_t *buffer_refs = NULL;
    int buffer_ref_count = 0;
    gltf_preload_buffer_view_ref_t *view_refs = NULL;
    int view_ref_count = 0;
    int ok = gltf_preload_stage_buffers(bundle,
                                        model_path,
                                        json,
                                        json_len,
                                        load_assets,
                                        &buffer_refs,
                                        &buffer_ref_count,
                                        error,
                                        error_cap);
    if (ok) {
        ok = gltf_preload_parse_buffer_views(
            json, json_len, &view_refs, &view_ref_count, error, error_cap);
    }
    if (ok) {
        ok = gltf_preload_validate_accessors(json,
                                             json_len,
                                             buffer_refs,
                                             buffer_ref_count,
                                             view_refs,
                                             view_ref_count,
                                             error,
                                             error_cap);
    }
    if (ok) {
        ok = gltf_preload_stage_meshes(bundle,
                                       json,
                                       json_len,
                                       buffer_refs,
                                       buffer_ref_count,
                                       view_refs,
                                       view_ref_count,
                                       error,
                                       error_cap);
    }
    if (ok) {
        ok = gltf_preload_stage_images(bundle,
                                       model_path,
                                       json,
                                       json_len,
                                       load_assets,
                                       buffer_refs,
                                       buffer_ref_count,
                                       view_refs,
                                       view_ref_count,
                                       error,
                                       error_cap);
    }
    free(buffer_refs);
    free(view_refs);
    return ok;
}

/// @brief Build an off-main-thread preload bundle from a C path and raw glTF/GLB bytes.
/// @details Extracts the JSON, validates accessors, and stages all dependencies so the subsequent
///          rt_gltf_load_preloaded_bundle call can build runtime objects on the main thread with no
///          file I/O or re-parsing. Takes ownership of @p root_data. On failure writes @p error,
///          frees partial work, and returns NULL.
/// @param[in] model_path Root asset path used to resolve external dependencies.
/// @param[in,out] root_data Owned raw glTF or GLB bytes; consumed on every return path.
/// @param root_size Size of @p root_data in bytes.
/// @param load_assets Non-zero to resolve external resources through the asset system.
/// @param[out] error Optional caller-provided error buffer, cleared before processing.
/// @param error_cap Capacity of @p error in bytes.
/// @return Populated preload bundle owned by the caller, or `NULL` on invalid input,
///         malformed content, allocation failure, or dependency-staging failure.
rt_gltf_preload_bundle *rt_gltf_preload_bundle_create_cstr(const char *model_path,
                                                           uint8_t *root_data,
                                                           size_t root_size,
                                                           int load_assets,
                                                           char *error,
                                                           size_t error_cap) {
    rt_gltf_preload_bundle *bundle;
    char *json = NULL;
    size_t json_len = 0;
    if (error && error_cap > 0)
        error[0] = '\0';
    if (!root_data || root_size == 0 || !model_path) {
        free(root_data);
        gltf_preload_set_error(error, error_cap, "failed to load model");
        return NULL;
    }
    bundle = gltf_preload_bundle_alloc_with_root(root_data, root_size, error, error_cap);
    if (!bundle) {
        free(root_data);
        return NULL;
    }
    json = gltf_json_copy_from_root_bytes(root_data, root_size, &json_len);
    if (!json) {
        gltf_preload_set_error(error, error_cap, "invalid glTF/GLB root document");
        rt_asset_error_set_if_empty(RT_ASSET_ERROR_CORRUPT,
                                    "GLTF.Preload: invalid glTF/GLB root document");
        rt_gltf_preload_bundle_free(bundle);
        return NULL;
    }
    if (!gltf_json_validate_document(json, json_len) ||
        !gltf_json_validate_gltf_integral_tokens(json, json_len) ||
        gltf_json_skip_ws(json, json_len, 0) >= json_len ||
        json[gltf_json_skip_ws(json, json_len, 0)] != '{') {
        free(json);
        gltf_preload_set_error(error, error_cap, "invalid glTF JSON document");
        rt_asset_error_set_if_empty(RT_ASSET_ERROR_CORRUPT,
                                    "GLTF.Preload: invalid glTF JSON document");
        rt_gltf_preload_bundle_free(bundle);
        return NULL;
    }
    {
        int ok = gltf_preload_stage_bundle_json(
            bundle, model_path, json, json_len, load_assets, error, error_cap);
        free(json);
        if (!ok) {
            rt_gltf_preload_bundle_free(bundle);
            return NULL;
        }
    }
    return bundle;
}

/// @brief Build an off-main-thread preload bundle from a runtime string path.
/// @details Thin compatibility wrapper around `rt_gltf_preload_bundle_create_cstr`; callers that
///          run on worker threads should prefer the C-string variant with an already-snapshotted
///          path.
/// @param path Runtime string path used to resolve external dependencies.
/// @param[in,out] root_data Owned raw glTF or GLB bytes; consumed by the delegated loader.
/// @param root_size Size of @p root_data in bytes.
/// @param load_assets Non-zero to resolve external resources through the asset system.
/// @param[out] error Optional caller-provided error buffer.
/// @param error_cap Capacity of @p error in bytes.
/// @return Resulting caller-owned preload bundle, or `NULL` on failure.
rt_gltf_preload_bundle *rt_gltf_preload_bundle_create(rt_string path,
                                                      uint8_t *root_data,
                                                      size_t root_size,
                                                      int load_assets,
                                                      char *error,
                                                      size_t error_cap) {
    return rt_gltf_preload_bundle_create_cstr(
        path ? rt_string_cstr(path) : NULL, root_data, root_size, load_assets, error, error_cap);
}

/// @brief Synthesize a name for an embedded resource (e.g. `inline-image-3.png`).
/// @details Used by the asset system to give meaningful names to inline base64 buffers.
/// @param[in] mime_type Optional MIME type used to choose a recognized image extension.
/// @param[in] fallback_ext Fallback extension, or `NULL` to use `.bin`.
/// @param[out] out Destination character buffer.
/// @param out_cap Capacity of @p out, including the terminator.
static void gltf_build_embedded_name(const char *mime_type,
                                     const char *fallback_ext,
                                     char *out,
                                     size_t out_cap) {
    const char *ext = fallback_ext ? fallback_ext : ".bin";
    if (!out || out_cap == 0)
        return;
    if (mime_type) {
        if (strstr(mime_type, "png"))
            ext = ".png";
        else if (strstr(mime_type, "jpeg") || strstr(mime_type, "jpg"))
            ext = ".jpg";
        else if (strstr(mime_type, "bmp"))
            ext = ".bmp";
        else if (strstr(mime_type, "gif"))
            ext = ".gif";
    }
    snprintf(out, out_cap, "embedded%s", ext);
}
