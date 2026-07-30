//===----------------------------------------------------------------------===//
// Part of the Zanna project, under the GNU GPL v3. See LICENSE.
// File: src/runtime/graphics/3d/assets/rt_gltf_material.inc  (#included from rt_gltf.c; not a
// standalone translation unit) Purpose: glTF image loading and PBR material/extension application
//===----------------------------------------------------------------------===//

/**
 * @file rt_gltf_material.inc
 * @brief Implements glTF image resolution and runtime PBR material construction.
 * @details Included by `rt_gltf.c`, this fragment resolves image sources from data URIs,
 *          bufferViews, external paths, and preload bundles; constructs texture/sampler lookup
 *          tables; maps core metallic-roughness materials; converts legacy specular-glossiness;
 *          and applies supported KHR material extensions. Missing required payloads are surfaced
 *          through the asset diagnostic channel while all temporary encoded bytes remain locally
 *          owned and are released by the calling phase.
 */

//===----------------------------------------------------------------------===//
// Loader phase helpers (extracted from rt_gltf_load_impl)
//===----------------------------------------------------------------------===//

/// @brief Transient source and ownership state used while loading one glTF image.
typedef struct {
    /// Borrowed declared or inferred MIME type.
    const char *mime_type;
    /// Optional owned encoded payload that must be freed after decoding.
    uint8_t *owned_data;
    /// Borrowed active encoded payload, possibly aliasing @ref owned_data or a bufferView.
    const uint8_t *image_data;
    /// Number of readable bytes at @ref image_data.
    size_t image_len;
    /// MIME type parsed from an inline data URI.
    char parsed_mime[64];
} gltf_image_load_state_t;

/// @brief Mark every image source referenced by a texture as required for material import.
/// @param textures_arr glTF JSON `textures` array.
/// @param texture_count Number of texture entries to inspect.
/// @param image_count Number of slots in @p image_required.
/// @param image_required Mutable per-image requirement flags.
static void gltf_mark_required_image_sources(void *textures_arr,
                                             int texture_count,
                                             int image_count,
                                             uint8_t *image_required) {
    if (!textures_arr || texture_count <= 0 || image_count <= 0 || !image_required)
        return;
    for (int i = 0; i < texture_count; i++) {
        void *texture_json = rt_seq_get(textures_arr, (int64_t)i);
        int64_t source_idx = gltf_texture_source_index(texture_json);
        if (source_idx >= 0 && source_idx < image_count)
            image_required[source_idx] = 1u;
    }
}

/// @brief Allocate image handles and requirement flags for the document's image/texture tables.
/// @param images_arr glTF JSON `images` array.
/// @param textures_arr glTF JSON `textures` array.
/// @param out_image_count Receives the validated image count.
/// @param out_texture_count Receives the validated texture count.
/// @param out_images Receives the zero-initialized image handle array.
/// @param out_image_required Receives referenced-image flags, or NULL when unnecessary.
/// @return Non-zero on success; zero after freeing all partial allocations.
static int gltf_prepare_image_arrays(void *images_arr,
                                     void *textures_arr,
                                     int *out_image_count,
                                     int *out_texture_count,
                                     void ***out_images,
                                     uint8_t **out_image_required) {
    int image_count = 0;
    int texture_count = 0;
    void **images = NULL;
    uint8_t *image_required = NULL;
    *out_image_count = 0;
    *out_texture_count = 0;
    *out_images = NULL;
    *out_image_required = NULL;
    if (!gltf_jarr_len_i32(images_arr, &image_count) ||
        !gltf_jarr_len_i32(textures_arr, &texture_count))
        return 0;
    if (image_count > 0) {
        images = (void **)calloc((size_t)image_count, sizeof(void *));
        if (!images)
            return 0;
    }
    if (image_count > 0 && texture_count > 0) {
        image_required = (uint8_t *)calloc((size_t)image_count, sizeof(uint8_t));
        if (!image_required) {
            free(images);
            return 0;
        }
        gltf_mark_required_image_sources(textures_arr, texture_count, image_count, image_required);
    }
    *out_image_count = image_count;
    *out_texture_count = texture_count;
    *out_images = images;
    *out_image_required = image_required;
    return 1;
}

/// @brief Resolve encoded bytes or a decoded object for one inline image data URI.
/// @param image_slot Destination runtime image slot.
/// @param preload_bundle Optional bundle containing staged encoded or decoded data.
/// @param image_index Source image index used in the preload key.
/// @param uri Inline data URI.
/// @param required_image Non-zero when a material references the image.
/// @param state Mutable source state receiving owned bytes and inferred MIME type.
/// @return Non-zero after resolution or warning emission.
static int gltf_load_inline_image_data(void **image_slot,
                                       rt_gltf_preload_bundle *preload_bundle,
                                       int image_index,
                                       const char *uri,
                                       int required_image,
                                       gltf_image_load_state_t *state) {
    char preload_key[96];
    gltf_data_uri_copy_mime(uri, state->parsed_mime, sizeof(state->parsed_mime));
    gltf_preload_image_key(image_index,
                           state->mime_type ? state->mime_type : state->parsed_mime,
                           preload_key,
                           sizeof(preload_key));
    *image_slot = gltf_preload_take_decoded_image(preload_bundle, preload_key);
    if (*image_slot)
        return 1;
    state->owned_data = gltf_preload_bundle_take_dependency(
        preload_bundle, preload_key, GLTF_PRELOAD_DEP_IMAGE, &state->image_len);
    if (state->owned_data) {
        state->image_data = state->owned_data;
        if (!state->mime_type && state->parsed_mime[0] != '\0')
            state->mime_type = state->parsed_mime;
        return 1;
    }
    if (gltf_parse_data_uri(uri,
                            state->parsed_mime,
                            sizeof(state->parsed_mime),
                            &state->owned_data,
                            &state->image_len)) {
        state->image_data = state->owned_data;
        if (!state->mime_type && state->parsed_mime[0] != '\0')
            state->mime_type = state->parsed_mime;
        return 1;
    }
    if (!required_image || !gltf_preload_image_is_supported_format(
                               state->mime_type ? state->mime_type : state->parsed_mime))
        return 1;
    rt_asset_error_add_warning("glTF inline texture not loaded: data URI decode failed");
    return 1;
}

/// @brief Resolve and decode one external image URI relative to the glTF document.
/// @param image_slot Destination runtime image slot.
/// @param filepath Path of the root glTF document.
/// @param load_assets Non-zero to consult the asset manager.
/// @param preload_bundle Optional source of staged image dependencies.
/// @param uri Relative image URI.
/// @param required_image Non-zero when a material references the image.
/// @return Non-zero after loading or reporting a recoverable missing-image warning.
static int gltf_load_external_image_ref(void **image_slot,
                                        const char *filepath,
                                        int load_assets,
                                        rt_gltf_preload_bundle *preload_bundle,
                                        const char *uri,
                                        int required_image) {
    char *image_path = gltf_resolve_relative_path_alloc(filepath, uri);
    const char *display_path = image_path ? image_path : uri;
    if (image_path)
        *image_slot = gltf_load_dependency_image(image_path, load_assets, preload_bundle);
    if (!*image_slot && required_image && gltf_preload_image_is_supported_format(display_path)) {
        rt_asset_error_add_warningf("glTF texture '%s' not loaded: %s",
                                    display_path,
                                    rt_asset_error_get_message()[0] ? rt_asset_error_get_message()
                                                                    : "content unavailable");
        free(image_path);
        return 1;
    }
    free(image_path);
    return 1;
}

/// @brief Resolve an embedded image payload from preload state or a glTF bufferView.
/// @param image_slot Destination runtime image slot.
/// @param root glTF JSON root used for bufferView resolution.
/// @param preload_bundle Optional bundle containing staged encoded or decoded data.
/// @param buffers Materialized glTF buffers.
/// @param buf_count Number of entries in @p buffers.
/// @param image_index Source image index used in the preload key.
/// @param image_json Image JSON object containing the bufferView reference.
/// @param required_image Non-zero when a material references the image.
/// @param state Mutable source state receiving payload bytes.
/// @return Non-zero after resolution or warning emission.
static int gltf_load_buffer_view_image_data(void **image_slot,
                                            void *root,
                                            rt_gltf_preload_bundle *preload_bundle,
                                            gltf_buffer_t *buffers,
                                            int buf_count,
                                            int image_index,
                                            void *image_json,
                                            int required_image,
                                            gltf_image_load_state_t *state) {
    int64_t view_idx = jint(image_json, "bufferView", -1);
    char preload_key[96];
    gltf_preload_image_key(image_index, state->mime_type, preload_key, sizeof(preload_key));
    *image_slot = gltf_preload_take_decoded_image(preload_bundle, preload_key);
    if (!*image_slot) {
        state->owned_data = gltf_preload_bundle_take_dependency(
            preload_bundle, preload_key, GLTF_PRELOAD_DEP_IMAGE, &state->image_len);
        if (state->owned_data)
            state->image_data = state->owned_data;
        else
            state->image_data =
                gltf_get_buffer_view_data(root, view_idx, buffers, buf_count, &state->image_len);
    }
    if (*image_slot || (state->image_data && state->image_len > 0) || !required_image ||
        !gltf_preload_image_is_supported_format(state->mime_type))
        return 1;
    rt_asset_error_add_warning("glTF embedded texture not loaded: image payload missing");
    return 1;
}

/// @brief Decode the encoded payload accumulated for one image into a runtime object.
/// @details Dispatches KTX2 through the texture loader, known raster formats through `VGRA`
///          staging, and other declared types through the asset decoder, preserving encoded source
///          bytes when supported.
/// @param image_slot Destination runtime image slot.
/// @param state Resolved encoded payload and MIME information.
/// @param required_image Non-zero when decode failure must be reported.
/// @return Non-zero when processing may continue; zero only when a required staged conversion
///         fails to produce an object.
static int gltf_decode_loaded_image(void **image_slot,
                                    const gltf_image_load_state_t *state,
                                    int required_image) {
    char image_name[64];
    const char *image_type;
    if (*image_slot || !state->image_data || state->image_len == 0)
        return 1;
    gltf_build_embedded_name(state->mime_type, ".bin", image_name, sizeof(image_name));
    image_type = state->mime_type ? state->mime_type : image_name;
    if (gltf_image_is_ktx2(image_type)) {
        *image_slot = gltf_decode_ktx2_payload(state->image_data, state->image_len);
        if (*image_slot || !required_image)
            return 1;
        rt_asset_error_add_warning("glTF embedded KTX2 texture not loaded: decode failed");
        return 1;
    }
    if (gltf_preload_image_is_supported_format(image_type)) {
        uint8_t *rgba_blob = NULL;
        size_t rgba_len = 0;
        if (gltf_decode_image_payload_to_rgba_blob(
                image_type, state->image_data, state->image_len, &rgba_blob, &rgba_len)) {
            *image_slot = gltf_pixels_from_rgba_blob(rgba_blob, rgba_len);
            free(rgba_blob);
            *image_slot = gltf_preserve_decoded_image_source(
                *image_slot, state->image_data, state->image_len);
            return *image_slot || !required_image;
        }
        if (required_image)
            rt_asset_error_add_warning("glTF embedded texture not loaded: image decode failed");
        return 1;
    }
    *image_slot = rt_asset_decode_typed(image_name, state->image_data, state->image_len);
    *image_slot =
        gltf_preserve_decoded_image_source(*image_slot, state->image_data, state->image_len);
    return 1;
}

/// @brief Load and decode one indexed glTF image from its URI or bufferView representation.
/// @param root glTF JSON root.
/// @param filepath Root document path used for relative image resolution.
/// @param load_assets Non-zero to consult the asset manager.
/// @param preload_bundle Optional staged dependency bundle.
/// @param buffers Materialized glTF buffers.
/// @param buf_count Number of entries in @p buffers.
/// @param images Destination runtime image array.
/// @param images_arr glTF JSON `images` array.
/// @param image_required Optional per-image requirement flags.
/// @param image_index Image index to process.
/// @return Non-zero when source resolution and decoding complete without a hard failure.
static int gltf_load_image_at_index(void *root,
                                    const char *filepath,
                                    int load_assets,
                                    rt_gltf_preload_bundle *preload_bundle,
                                    gltf_buffer_t *buffers,
                                    int buf_count,
                                    void **images,
                                    void *images_arr,
                                    const uint8_t *image_required,
                                    int image_index) {
    void *image_json = rt_seq_get(images_arr, (int64_t)image_index);
    const char *uri = jstr(image_json, "uri");
    gltf_image_load_state_t state;
    int required_image = image_required ? image_required[image_index] != 0 : 0;
    int ok;
    memset(&state, 0, sizeof(state));
    state.mime_type = jstr(image_json, "mimeType");
    if (uri && strncmp(uri, "data:", 5) == 0) {
        ok = gltf_load_inline_image_data(
            &images[image_index], preload_bundle, image_index, uri, required_image, &state);
    } else if (uri) {
        ok = gltf_load_external_image_ref(
            &images[image_index], filepath, load_assets, preload_bundle, uri, required_image);
    } else {
        ok = gltf_load_buffer_view_image_data(&images[image_index],
                                              root,
                                              preload_bundle,
                                              buffers,
                                              buf_count,
                                              image_index,
                                              image_json,
                                              required_image,
                                              &state);
    }
    if (ok)
        ok = gltf_decode_loaded_image(&images[image_index], &state, required_image);
    free(state.owned_data);
    return ok;
}

/// @brief Allocate parallel image, support, and sampler tables for glTF textures.
/// @param texture_count Number of texture entries.
/// @param out_texture_images Receives the zero-initialized image-handle table.
/// @param out_texture_supported Receives per-texture supported-format flags.
/// @param out_texture_samplers Receives per-texture sampler descriptions.
/// @return Non-zero on success or for an empty table; zero after cleaning partial allocations.
static int gltf_alloc_texture_tables(int texture_count,
                                     void ***out_texture_images,
                                     uint8_t **out_texture_supported,
                                     gltf_sampler_info_t **out_texture_samplers) {
    void **texture_images = NULL;
    uint8_t *texture_supported = NULL;
    gltf_sampler_info_t *texture_samplers = NULL;
    *out_texture_images = NULL;
    *out_texture_supported = NULL;
    *out_texture_samplers = NULL;
    if (texture_count <= 0)
        return 1;
    texture_images = (void **)calloc((size_t)texture_count, sizeof(void *));
    texture_supported = (uint8_t *)calloc((size_t)texture_count, sizeof(uint8_t));
    texture_samplers =
        (gltf_sampler_info_t *)calloc((size_t)texture_count, sizeof(*texture_samplers));
    if (!texture_images || !texture_supported || !texture_samplers) {
        free(texture_images);
        free(texture_supported);
        free(texture_samplers);
        return 0;
    }
    *out_texture_images = texture_images;
    *out_texture_supported = texture_supported;
    *out_texture_samplers = texture_samplers;
    return 1;
}

/// @brief Determine whether an image JSON object declares a texture format the importer supports.
/// @param image_json glTF image object to classify by MIME type, data URI, or filename.
/// @return Non-zero for PNG, BMP, JPEG, GIF, or KTX2.
static int gltf_image_json_supported_for_texture(void *image_json) {
    const char *image_uri = jstr(image_json, "uri");
    const char *image_mime = jstr(image_json, "mimeType");
    char parsed_mime[64];
    const char *image_type;
    parsed_mime[0] = '\0';
    if (image_uri && strncmp(image_uri, "data:", 5) == 0)
        gltf_data_uri_copy_mime(image_uri, parsed_mime, sizeof(parsed_mime));
    image_type = image_mime ? image_mime : (parsed_mime[0] != '\0' ? parsed_mime : image_uri);
    return gltf_preload_image_is_supported_format(image_type) || gltf_image_is_ktx2(image_type);
}

/// @brief Populate texture-to-image bindings, format flags, and sampler descriptions.
/// @param root glTF JSON root containing sampler definitions.
/// @param images_arr glTF JSON `images` array.
/// @param image_count Number of entries in @p images.
/// @param images Resolved runtime image handles.
/// @param textures_arr glTF JSON `textures` array.
/// @param texture_count Number of texture entries to populate.
/// @param texture_images Destination image handles indexed by texture.
/// @param texture_supported Destination supported-format flags.
/// @param texture_samplers Destination sampler descriptions.
static void gltf_populate_texture_tables(void *root,
                                         void *images_arr,
                                         int image_count,
                                         void **images,
                                         void *textures_arr,
                                         int texture_count,
                                         void **texture_images,
                                         uint8_t *texture_supported,
                                         gltf_sampler_info_t *texture_samplers) {
    void *samplers_arr = jarr(root, "samplers");
    for (int i = 0; i < texture_count && texture_images; i++) {
        void *texture_json = rt_seq_get(textures_arr, (int64_t)i);
        int64_t source_idx = gltf_texture_source_index(texture_json);
        int64_t sampler_idx = jint(texture_json, "sampler", -1);
        if (texture_samplers) {
            void *sampler_json = sampler_idx >= 0 && sampler_idx < jarr_len(samplers_arr)
                                     ? rt_seq_get(samplers_arr, sampler_idx)
                                     : NULL;
            gltf_read_sampler_info(sampler_json, &texture_samplers[i]);
        }
        if (images && source_idx >= 0 && source_idx < image_count) {
            void *image_json = rt_seq_get(images_arr, source_idx);
            if (texture_supported && gltf_image_json_supported_for_texture(image_json))
                texture_supported[i] = 1u;
            texture_images[i] = images[source_idx];
        }
    }
}

/// @brief Decode glTF images and resolve textures (sampler info + image binding)
///        into the parallel texture tables consumed by material resolution.
///        Extracted post-asset phase of rt_gltf_load_impl; never early-returns.
/// @note All seven out-params (images/image_count/image_required and the three
///       texture tables + texture_count) are heap state owned by the caller's
///       cleanup. load_failed_io is set when a required image fails to decode.
/// @param root glTF JSON root.
/// @param filepath Root document path for relative dependencies.
/// @param load_assets Non-zero to consult the asset manager.
/// @param preload_bundle Optional staged dependency bundle.
/// @param buffers Materialized glTF buffers.
/// @param buf_count Number of entries in @p buffers.
/// @param out_images Receives the runtime image-handle array.
/// @param out_image_count Receives the number of image slots.
/// @param out_image_required Receives per-image material-reference flags.
/// @param out_texture_images Receives image handles indexed by glTF texture.
/// @param out_texture_supported Receives supported-format flags indexed by texture.
/// @param out_texture_samplers Receives sampler descriptions indexed by texture.
/// @param out_texture_count Receives the number of texture slots.
/// @param load_failed_io In/out accumulated hard-failure flag.
static void gltf_load_images_and_textures(void *root,
                                          const char *filepath,
                                          int load_assets,
                                          rt_gltf_preload_bundle *preload_bundle,
                                          gltf_buffer_t *buffers,
                                          int buf_count,
                                          void ***out_images,
                                          int *out_image_count,
                                          uint8_t **out_image_required,
                                          void ***out_texture_images,
                                          uint8_t **out_texture_supported,
                                          gltf_sampler_info_t **out_texture_samplers,
                                          int *out_texture_count,
                                          int *load_failed_io) {
    int load_failed = *load_failed_io;
    void **images = NULL;
    void **texture_images = NULL;
    uint8_t *texture_supported = NULL;
    gltf_sampler_info_t *texture_samplers = NULL;
    void *images_arr = jarr(root, "images");
    int image_count = 0;
    void *textures_arr = jarr(root, "textures");
    int texture_count = 0;
    uint8_t *image_required = NULL;
    if (!gltf_prepare_image_arrays(
            images_arr, textures_arr, &image_count, &texture_count, &images, &image_required)) {
        load_failed = 1;
        goto finish;
    }

    for (int i = 0; i < image_count && images; i++) {
        if (!gltf_load_image_at_index(root,
                                      filepath,
                                      load_assets,
                                      preload_bundle,
                                      buffers,
                                      buf_count,
                                      images,
                                      images_arr,
                                      image_required,
                                      i)) {
            load_failed = 1;
            break;
        }
    }
    if (load_failed)
        goto finish;

    if (!gltf_alloc_texture_tables(
            texture_count, &texture_images, &texture_supported, &texture_samplers)) {
        load_failed = 1;
        goto finish;
    }
    gltf_populate_texture_tables(root,
                                 images_arr,
                                 image_count,
                                 images,
                                 textures_arr,
                                 texture_count,
                                 texture_images,
                                 texture_supported,
                                 texture_samplers);
finish:
    *out_images = images;
    *out_image_count = image_count;
    *out_image_required = image_required;
    *out_texture_images = texture_images;
    *out_texture_supported = texture_supported;
    *out_texture_samplers = texture_samplers;
    *out_texture_count = texture_count;
    *load_failed_io = load_failed;
}

/// @brief Runtime callback that assigns a resolved texture object to one material channel.
/// @param material Runtime material receiving the texture.
/// @param texture Resolved image or texture object.
typedef void (*gltf_material_texture_setter_fn)(void *material, void *texture);

/// @brief Resolve one material texture-slot metadata record.
/// @param material_infos Per-material metadata table.
/// @param material_index Material index to address.
/// @param slot Runtime texture-slot index.
/// @return Mutable slot record, or NULL for an invalid table or index.
static gltf_texture_info_t *gltf_material_slot_info(gltf_material_info_t *material_infos,
                                                    int32_t material_index,
                                                    int32_t slot) {
    if (!material_infos || material_index < 0 || slot < 0 ||
        slot >= RT_MATERIAL3D_TEXTURE_SLOT_COUNT)
        return NULL;
    return &material_infos[material_index].slots[slot];
}

/// @brief Resolve, bind, and apply sampling metadata for one material texture reference.
/// @param texture_json glTF texture-info object containing the texture index and transforms.
/// @param material Runtime material to update.
/// @param slot Runtime texture-slot index, or a negative value for validation only.
/// @param texture_images Resolved image handles indexed by glTF texture.
/// @param texture_supported Supported-format flags indexed by texture.
/// @param texture_samplers Sampler descriptions indexed by texture.
/// @param texture_count Number of entries in the parallel texture tables.
/// @param slot_info Optional metadata record populated from @p texture_json.
/// @param setter Optional runtime texture-assignment callback.
/// @return Zero after successful binding, optional absence, or warning emission.
static int gltf_material_bind_texture(void *texture_json,
                                      void *material,
                                      int32_t slot,
                                      void **texture_images,
                                      const uint8_t *texture_supported,
                                      const gltf_sampler_info_t *texture_samplers,
                                      int32_t texture_count,
                                      gltf_texture_info_t *slot_info,
                                      gltf_material_texture_setter_fn setter) {
    int64_t tex_idx = jint(texture_json, "index", -1);
    if (texture_json && slot_info)
        gltf_read_texture_info(texture_json, slot_info);
    if (tex_idx >= 0 && tex_idx < texture_count && texture_images && texture_images[tex_idx]) {
        if (setter)
            setter(material, texture_images[tex_idx]);
    } else if (gltf_texture_index_missing_supported_payload(
                   tex_idx, texture_count, texture_images, texture_supported)) {
        rt_asset_error_add_warning("glTF material texture left untextured: image payload missing");
        return 0;
    }
    gltf_apply_texture_slot(texture_samplers, texture_count, tex_idx, material, slot, slot_info);
    return 0;
}

/// @brief Read and sanitize `baseColorFactor` with the glTF identity default.
/// @param pbr glTF `pbrMetallicRoughness` object.
/// @param rgba Destination four-component linear color and alpha.
static void gltf_material_read_pbr_base_color(void *pbr, double rgba[4]) {
    void *bcf = jarr(pbr, "baseColorFactor");
    rgba[0] = 1.0;
    rgba[1] = 1.0;
    rgba[2] = 1.0;
    rgba[3] = 1.0;
    if (bcf && jarr_len(bcf) >= 3) {
        rgba[0] = gltf_clamp_double(jvalue_num(rt_seq_get(bcf, 0), rgba[0]), 0.0, 1.0, rgba[0]);
        rgba[1] = gltf_clamp_double(jvalue_num(rt_seq_get(bcf, 1), rgba[1]), 0.0, 1.0, rgba[1]);
        rgba[2] = gltf_clamp_double(jvalue_num(rt_seq_get(bcf, 2), rgba[2]), 0.0, 1.0, rgba[2]);
        if (jarr_len(bcf) >= 4)
            rgba[3] = gltf_clamp_double(jvalue_num(rt_seq_get(bcf, 3), rgba[3]), 0.0, 1.0, rgba[3]);
    }
}

/// @brief Construct a runtime PBR material from core glTF metallic-roughness factors.
/// @param pbr glTF `pbrMetallicRoughness` object.
/// @return Newly retained material, or NULL when input or allocation is invalid.
static void *gltf_material_create_pbr_from_json(void *pbr) {
    double rgba[4];
    double metallic;
    double roughness;
    void *mat;
    if (!pbr)
        return NULL;
    gltf_material_read_pbr_base_color(pbr, rgba);
    metallic = gltf_clamp_double(jnum(pbr, "metallicFactor", 1.0), 0.0, 1.0, 1.0);
    roughness = gltf_clamp_double(jnum(pbr, "roughnessFactor", 1.0), 0.0, 1.0, 1.0);
    mat = rt_material3d_new_pbr(rgba[0], rgba[1], rgba[2]);
    if (mat) {
        rt_material3d_set_metallic(mat, metallic);
        rt_material3d_set_roughness(mat, roughness);
        if (rgba[3] < 1.0)
            rt_material3d_set_alpha(mat, rgba[3]);
    }
    return mat;
}

/// @brief Plan 09: convert a KHR_materials_pbrSpecularGlossiness payload into a
///        metallic-roughness Material3D.
/// @details Standard approximation implemented from the published math:
///          `diffuseFactor` → base color, `roughness = 1 - glossinessFactor`, and
///          `metallic ≈ saturate((maxComponent(specularFactor) - 0.04) / (1 - 0.04))`
///          (dielectric F0 = 0.04). When a specularGlossinessTexture is present and
///          decodes to CPU pixels, gltf_material_synthesize_spec_gloss_mr_map upgrades
///          this factor-level conversion to a per-texel metallic-roughness map.
/// @param spec_gloss `KHR_materials_pbrSpecularGlossiness` extension object.
/// @return Newly retained metallic-roughness material, or NULL for absent input/allocation failure.
static void *gltf_material_create_spec_gloss_from_json(void *spec_gloss) {
    double rgba[4] = {1.0, 1.0, 1.0, 1.0};
    double spec[3] = {1.0, 1.0, 1.0};
    double gloss;
    double max_spec;
    double metallic;
    void *mat;
    void *df;
    void *sf;
    if (!spec_gloss)
        return NULL;
    df = jarr(spec_gloss, "diffuseFactor");
    if (df && jarr_len(df) >= 3) {
        rgba[0] = gltf_clamp_double(jvalue_num(rt_seq_get(df, 0), rgba[0]), 0.0, 1.0, rgba[0]);
        rgba[1] = gltf_clamp_double(jvalue_num(rt_seq_get(df, 1), rgba[1]), 0.0, 1.0, rgba[1]);
        rgba[2] = gltf_clamp_double(jvalue_num(rt_seq_get(df, 2), rgba[2]), 0.0, 1.0, rgba[2]);
        if (jarr_len(df) >= 4)
            rgba[3] = gltf_clamp_double(jvalue_num(rt_seq_get(df, 3), rgba[3]), 0.0, 1.0, rgba[3]);
    }
    gloss = gltf_clamp_double(jnum(spec_gloss, "glossinessFactor", 1.0), 0.0, 1.0, 1.0);
    sf = jarr(spec_gloss, "specularFactor");
    if (sf && jarr_len(sf) >= 3) {
        spec[0] = gltf_clamp_double(jvalue_num(rt_seq_get(sf, 0), spec[0]), 0.0, 1.0, spec[0]);
        spec[1] = gltf_clamp_double(jvalue_num(rt_seq_get(sf, 1), spec[1]), 0.0, 1.0, spec[1]);
        spec[2] = gltf_clamp_double(jvalue_num(rt_seq_get(sf, 2), spec[2]), 0.0, 1.0, spec[2]);
    }
    max_spec = spec[0];
    if (spec[1] > max_spec)
        max_spec = spec[1];
    if (spec[2] > max_spec)
        max_spec = spec[2];
    metallic = (max_spec - 0.04) / (1.0 - 0.04);
    if (metallic < 0.0)
        metallic = 0.0;
    if (metallic > 1.0)
        metallic = 1.0;
    mat = rt_material3d_new_pbr(rgba[0], rgba[1], rgba[2]);
    if (mat) {
        rt_material3d_set_metallic(mat, metallic);
        rt_material3d_set_roughness(mat, 1.0 - gloss);
        if (rgba[3] < 1.0)
            rt_material3d_set_alpha(mat, rgba[3]);
    }
    return mat;
}

/// @brief Plan 09: bind the spec-gloss extension's textures (diffuse → base color,
///        specularGlossiness → specular slot).
/// @param spec_gloss `KHR_materials_pbrSpecularGlossiness` extension object.
/// @param material Runtime material receiving the texture assignments.
/// @param material_index Index used to locate per-slot metadata.
/// @param texture_images Resolved images indexed by glTF texture.
/// @param texture_supported Supported-format flags indexed by texture.
/// @param texture_samplers Sampler descriptions indexed by texture.
/// @param texture_count Number of entries in the parallel texture tables.
/// @param material_infos Per-material texture metadata table.
/// @return Accumulated hard-failure indicator from texture resolution.
static int gltf_material_apply_spec_gloss_textures(void *spec_gloss,
                                                   void *material,
                                                   int32_t material_index,
                                                   void **texture_images,
                                                   const uint8_t *texture_supported,
                                                   const gltf_sampler_info_t *texture_samplers,
                                                   int32_t texture_count,
                                                   gltf_material_info_t *material_infos) {
    int load_failed = 0;
    void *diffuse_tex = jget(spec_gloss, "diffuseTexture");
    void *sg_tex = jget(spec_gloss, "specularGlossinessTexture");
    load_failed |= gltf_material_bind_texture(
        diffuse_tex,
        material,
        RT_MATERIAL3D_TEXTURE_SLOT_BASE_COLOR,
        texture_images,
        texture_supported,
        texture_samplers,
        texture_count,
        gltf_material_slot_info(
            material_infos, material_index, RT_MATERIAL3D_TEXTURE_SLOT_BASE_COLOR),
        rt_material3d_set_texture);
    load_failed |= gltf_material_bind_texture(
        sg_tex,
        material,
        RT_MATERIAL3D_TEXTURE_SLOT_SPECULAR,
        texture_images,
        texture_supported,
        texture_samplers,
        texture_count,
        gltf_material_slot_info(
            material_infos, material_index, RT_MATERIAL3D_TEXTURE_SLOT_SPECULAR),
        rt_material3d_set_specular_map);
    return load_failed;
}

/// @brief Convert one 8-bit sRGB-encoded channel to linear [0,1] (IEC 61966-2-1).
/// @param value sRGB channel value from zero through 255.
/// @return Corresponding linear-light channel in [0, 1].
static double gltf_srgb_u8_to_linear(uint8_t value) {
    double c = (double)value / 255.0;
    if (c <= 0.04045)
        return c / 12.92;
    return pow((c + 0.055) / 1.055, 2.4);
}

/// @brief Clamp @p value to [0,1] and quantize to an 8-bit channel with rounding.
/// @param value Candidate normalized channel.
/// @return Quantized channel from zero through 255.
static uint8_t gltf_unit_to_u8(double value) {
    if (!isfinite(value) || value <= 0.0)
        return 0u;
    if (value >= 1.0)
        return 255u;
    return (uint8_t)(value * 255.0 + 0.5);
}

/// @brief Per-texel spec-gloss → metallic-roughness conversion (completes Plan 09 §3.2).
/// @details When the specularGlossinessTexture resolves to CPU pixels, synthesize a
///          metallic-roughness Pixels in the glTF convention (roughness in G, metallic in
///          B; R and A left at 255) with the material factors baked into the texels:
///          `roughness = 1 - glossinessFactor * glossTexel` and metallic from the max
///          component of `specularFactor * linear(specTexel)` against dielectric F0 0.04.
///          The synthesized map binds to the metallic-roughness slot with the source
///          texture's sampler/texcoord state, and the scalar metallic/roughness reset to 1
///          so the baked texels are not scaled twice. Compressed (non-Pixels) sources keep
///          the factor-level approximation.
/// @param spec_gloss `KHR_materials_pbrSpecularGlossiness` extension object.
/// @param material Runtime material receiving the synthesized map.
/// @param material_index Index used to copy per-slot texture metadata.
/// @param texture_images Resolved texture image table.
/// @param texture_samplers Sampler descriptions indexed by texture.
/// @param texture_count Number of entries in the texture tables.
/// @param material_infos Per-material texture metadata table.
static void gltf_material_synthesize_spec_gloss_mr_map(void *spec_gloss,
                                                       void *material,
                                                       int32_t material_index,
                                                       void **texture_images,
                                                       const gltf_sampler_info_t *texture_samplers,
                                                       int32_t texture_count,
                                                       gltf_material_info_t *material_infos) {
    void *sg_tex = spec_gloss ? jget(spec_gloss, "specularGlossinessTexture") : NULL;
    int64_t tex_idx = jint(sg_tex, "index", -1);
    rt_pixels_impl *src;
    rt_pixels_impl *mr;
    double gloss_factor;
    double spec_factor[3] = {1.0, 1.0, 1.0};
    void *sf;
    size_t pixel_count;
    if (!material || !texture_images || tex_idx < 0 || tex_idx >= texture_count)
        return;
    src = rt_pixels_checked_impl_or_null(
        rt_material3d_resolve_texture_pixels(texture_images[tex_idx]));
    if (!src || !src->data || src->width <= 0 || src->height <= 0)
        return;
    gloss_factor = gltf_clamp_double(jnum(spec_gloss, "glossinessFactor", 1.0), 0.0, 1.0, 1.0);
    sf = jarr(spec_gloss, "specularFactor");
    if (sf && jarr_len(sf) >= 3) {
        for (int i = 0; i < 3; i++)
            spec_factor[i] = gltf_clamp_double(jvalue_num(rt_seq_get(sf, i), 1.0), 0.0, 1.0, 1.0);
    }
    mr = pixels_alloc(src->width, src->height);
    if (!mr)
        return;
    pixel_count = (size_t)src->width * (size_t)src->height;
    for (size_t i = 0; i < pixel_count; i++) {
        uint32_t texel = src->data[i]; /* 0xRRGGBBAA */
        double spec_r = gltf_srgb_u8_to_linear((uint8_t)((texel >> 24) & 0xFFu)) * spec_factor[0];
        double spec_g = gltf_srgb_u8_to_linear((uint8_t)((texel >> 16) & 0xFFu)) * spec_factor[1];
        double spec_b = gltf_srgb_u8_to_linear((uint8_t)((texel >> 8) & 0xFFu)) * spec_factor[2];
        double gloss = gloss_factor * ((double)(texel & 0xFFu) / 255.0);
        double max_spec = spec_r;
        double metallic;
        uint8_t roughness_u8;
        uint8_t metallic_u8;
        if (spec_g > max_spec)
            max_spec = spec_g;
        if (spec_b > max_spec)
            max_spec = spec_b;
        metallic = (max_spec - 0.04) / (1.0 - 0.04);
        roughness_u8 = gltf_unit_to_u8(1.0 - gloss);
        metallic_u8 = gltf_unit_to_u8(metallic);
        mr->data[i] =
            0xFF000000u | ((uint32_t)roughness_u8 << 16) | ((uint32_t)metallic_u8 << 8) | 0xFFu;
    }
    pixels_touch(mr);
    rt_material3d_set_metallic_roughness_map(material, mr);
    {
        /* The MR map samples with the SG texture's texcoord/transform and sampler state. */
        gltf_texture_info_t *sg_info = gltf_material_slot_info(
            material_infos, material_index, RT_MATERIAL3D_TEXTURE_SLOT_SPECULAR);
        gltf_texture_info_t *mr_info = gltf_material_slot_info(
            material_infos, material_index, RT_MATERIAL3D_TEXTURE_SLOT_METALLIC_ROUGHNESS);
        if (sg_info && mr_info)
            *mr_info = *sg_info;
        gltf_apply_texture_slot(texture_samplers,
                                texture_count,
                                tex_idx,
                                material,
                                RT_MATERIAL3D_TEXTURE_SLOT_METALLIC_ROUGHNESS,
                                mr_info);
    }
    rt_material3d_set_metallic(material, 1.0);
    rt_material3d_set_roughness(material, 1.0);
    gltf_release_local(mr);
}

/// @brief Construct the default glTF metallic-roughness material.
/// @return Newly retained white material with metallic and roughness factors of one, or NULL.
static void *gltf_material_create_default_pbr(void) {
    void *mat = rt_material3d_new_pbr(1.0, 1.0, 1.0);
    if (mat) {
        rt_material3d_set_metallic(mat, 1.0);
        rt_material3d_set_roughness(mat, 1.0);
    }
    return mat;
}

/// @brief Bind core base-color and metallic-roughness textures to a PBR material.
/// @param pbr glTF `pbrMetallicRoughness` object.
/// @param material Runtime material receiving textures.
/// @param material_index Index used to locate per-slot metadata.
/// @param texture_images Resolved images indexed by texture.
/// @param texture_supported Supported-format flags indexed by texture.
/// @param texture_samplers Sampler descriptions indexed by texture.
/// @param texture_count Number of entries in the texture tables.
/// @param material_infos Per-material texture metadata table.
/// @return Accumulated hard-failure indicator from texture resolution.
static int gltf_material_apply_pbr_textures(void *pbr,
                                            void *material,
                                            int32_t material_index,
                                            void **texture_images,
                                            const uint8_t *texture_supported,
                                            const gltf_sampler_info_t *texture_samplers,
                                            int32_t texture_count,
                                            gltf_material_info_t *material_infos) {
    int load_failed = 0;
    void *base_tex = jget(pbr, "baseColorTexture");
    void *mr_tex = jget(pbr, "metallicRoughnessTexture");
    load_failed |= gltf_material_bind_texture(
        base_tex,
        material,
        RT_MATERIAL3D_TEXTURE_SLOT_BASE_COLOR,
        texture_images,
        texture_supported,
        texture_samplers,
        texture_count,
        gltf_material_slot_info(
            material_infos, material_index, RT_MATERIAL3D_TEXTURE_SLOT_BASE_COLOR),
        rt_material3d_set_texture);
    load_failed |= gltf_material_bind_texture(
        mr_tex,
        material,
        RT_MATERIAL3D_TEXTURE_SLOT_METALLIC_ROUGHNESS,
        texture_images,
        texture_supported,
        texture_samplers,
        texture_count,
        gltf_material_slot_info(
            material_infos, material_index, RT_MATERIAL3D_TEXTURE_SLOT_METALLIC_ROUGHNESS),
        rt_material3d_set_metallic_roughness_map);
    return load_failed;
}

/// @brief Apply a material's optional three-component emissive factor.
/// @param mat_json glTF material object.
/// @param material Runtime material to update.
static void gltf_material_apply_emissive_factor(void *mat_json, void *material) {
    void *ef = jarr(mat_json, "emissiveFactor");
    if (ef && jarr_len(ef) >= 3) {
        double er = gltf_clamp_double(jvalue_num(rt_seq_get(ef, 0), 0.0), 0.0, 1.0, 0.0);
        double eg = gltf_clamp_double(jvalue_num(rt_seq_get(ef, 1), 0.0), 0.0, 1.0, 0.0);
        double eb = gltf_clamp_double(jvalue_num(rt_seq_get(ef, 2), 0.0), 0.0, 1.0, 0.0);
        rt_material3d_set_emissive_color(material, er, eg, eb);
    }
}

/// @brief Apply `KHR_materials_specular` factors and the preferred specular texture.
/// @param specular Extension object, or NULL.
/// @param material Runtime material to update.
/// @param material_index Index used for per-slot metadata.
/// @param texture_images Resolved images indexed by texture.
/// @param texture_supported Supported-format flags indexed by texture.
/// @param texture_samplers Sampler descriptions indexed by texture.
/// @param texture_count Number of texture entries.
/// @param material_infos Per-material texture metadata table.
/// @return Accumulated hard-failure indicator from texture resolution.
static int gltf_material_apply_specular_extension(void *specular,
                                                  void *material,
                                                  int32_t material_index,
                                                  void **texture_images,
                                                  const uint8_t *texture_supported,
                                                  const gltf_sampler_info_t *texture_samplers,
                                                  int32_t texture_count,
                                                  gltf_material_info_t *material_infos) {
    void *spec_color;
    void *spec_tex;
    void *spec_color_tex;
    void *chosen_spec_tex;
    if (!specular || !material)
        return 0;
    spec_color = jarr(specular, "specularColorFactor");
    spec_tex = jget(specular, "specularTexture");
    spec_color_tex = jget(specular, "specularColorTexture");
    chosen_spec_tex = spec_color_tex ? spec_color_tex : spec_tex;
    ((rt_material3d *)material)->specular[0] = gltf_clamp_double(
        jnum(specular, "specularFactor", ((rt_material3d *)material)->specular[0]),
        0.0,
        1.0,
        ((rt_material3d *)material)->specular[0]);
    ((rt_material3d *)material)->specular[1] = ((rt_material3d *)material)->specular[0];
    ((rt_material3d *)material)->specular[2] = ((rt_material3d *)material)->specular[0];
    if (spec_color && jarr_len(spec_color) >= 3) {
        ((rt_material3d *)material)->specular[0] = gltf_clamp_double(
            jvalue_num(rt_seq_get(spec_color, 0), ((rt_material3d *)material)->specular[0]),
            0.0,
            1.0,
            ((rt_material3d *)material)->specular[0]);
        ((rt_material3d *)material)->specular[1] = gltf_clamp_double(
            jvalue_num(rt_seq_get(spec_color, 1), ((rt_material3d *)material)->specular[1]),
            0.0,
            1.0,
            ((rt_material3d *)material)->specular[1]);
        ((rt_material3d *)material)->specular[2] = gltf_clamp_double(
            jvalue_num(rt_seq_get(spec_color, 2), ((rt_material3d *)material)->specular[2]),
            0.0,
            1.0,
            ((rt_material3d *)material)->specular[2]);
    }
    return gltf_material_bind_texture(chosen_spec_tex,
                                      material,
                                      RT_MATERIAL3D_TEXTURE_SLOT_SPECULAR,
                                      texture_images,
                                      texture_supported,
                                      texture_samplers,
                                      texture_count,
                                      gltf_material_slot_info(material_infos,
                                                              material_index,
                                                              RT_MATERIAL3D_TEXTURE_SLOT_SPECULAR),
                                      rt_material3d_set_specular_map);
}

/// @brief Validate an extension texture reference without assigning it to a runtime slot.
/// @param texture_json Extension texture-info object.
/// @param material Runtime material passed through the shared binding path.
/// @param texture_images Resolved images indexed by texture.
/// @param texture_supported Supported-format flags indexed by texture.
/// @param texture_samplers Sampler descriptions indexed by texture.
/// @param texture_count Number of texture entries.
/// @return Accumulated hard-failure indicator from texture resolution.
static int gltf_material_validate_extension_texture(void *texture_json,
                                                    void *material,
                                                    void **texture_images,
                                                    const uint8_t *texture_supported,
                                                    const gltf_sampler_info_t *texture_samplers,
                                                    int32_t texture_count) {
    return gltf_material_bind_texture(texture_json,
                                      material,
                                      -1,
                                      texture_images,
                                      texture_supported,
                                      texture_samplers,
                                      texture_count,
                                      NULL,
                                      NULL);
}

/// @brief Apply clearcoat factors and validate all clearcoat texture references.
/// @param clearcoat `KHR_materials_clearcoat` object, or NULL.
/// @param material Runtime material to update.
/// @param texture_images Resolved images indexed by texture.
/// @param texture_supported Supported-format flags indexed by texture.
/// @param texture_samplers Sampler descriptions indexed by texture.
/// @param texture_count Number of texture entries.
/// @return Accumulated hard-failure indicator from extension texture resolution.
static int gltf_material_apply_clearcoat_extension(void *clearcoat,
                                                   void *material,
                                                   void **texture_images,
                                                   const uint8_t *texture_supported,
                                                   const gltf_sampler_info_t *texture_samplers,
                                                   int32_t texture_count) {
    double clearcoat_factor;
    double clearcoat_roughness;
    int load_failed = 0;
    if (!clearcoat || !material)
        return 0;
    clearcoat_factor = gltf_clamp_double(jnum(clearcoat, "clearcoatFactor", 0.0), 0.0, 1.0, 0.0);
    clearcoat_roughness =
        gltf_clamp_double(jnum(clearcoat, "clearcoatRoughnessFactor", 0.0), 0.0, 1.0, 0.0);
    if (clearcoat_factor > ((rt_material3d *)material)->reflectivity)
        rt_material3d_set_reflectivity(material, clearcoat_factor);
    rt_material3d_set_custom_param(material, 1, clearcoat_factor);
    rt_material3d_set_custom_param(material, 2, clearcoat_roughness);
    load_failed |= gltf_material_validate_extension_texture(jget(clearcoat, "clearcoatTexture"),
                                                            material,
                                                            texture_images,
                                                            texture_supported,
                                                            texture_samplers,
                                                            texture_count);
    load_failed |=
        gltf_material_validate_extension_texture(jget(clearcoat, "clearcoatRoughnessTexture"),
                                                 material,
                                                 texture_images,
                                                 texture_supported,
                                                 texture_samplers,
                                                 texture_count);
    load_failed |=
        gltf_material_validate_extension_texture(jget(clearcoat, "clearcoatNormalTexture"),
                                                 material,
                                                 texture_images,
                                                 texture_supported,
                                                 texture_samplers,
                                                 texture_count);
    return load_failed;
}

/// @brief Apply transmission strength and validate its optional texture reference.
/// @param transmission `KHR_materials_transmission` object, or NULL.
/// @param material Runtime material to update.
/// @param texture_images Resolved images indexed by texture.
/// @param texture_supported Supported-format flags indexed by texture.
/// @param texture_samplers Sampler descriptions indexed by texture.
/// @param texture_count Number of texture entries.
/// @return Accumulated hard-failure indicator from extension texture resolution.
static int gltf_material_apply_transmission_extension(void *transmission,
                                                      void *material,
                                                      void **texture_images,
                                                      const uint8_t *texture_supported,
                                                      const gltf_sampler_info_t *texture_samplers,
                                                      int32_t texture_count) {
    double transmission_factor;
    if (!transmission || !material)
        return 0;
    transmission_factor =
        gltf_clamp_double(jnum(transmission, "transmissionFactor", 0.0), 0.0, 1.0, 0.0);
    if (transmission_factor > 0.0) {
        if (transmission_factor > ((rt_material3d *)material)->reflectivity)
            rt_material3d_set_reflectivity(material, transmission_factor);
        rt_material3d_set_custom_param(material, 3, transmission_factor);
    }
    return gltf_material_validate_extension_texture(jget(transmission, "transmissionTexture"),
                                                    material,
                                                    texture_images,
                                                    texture_supported,
                                                    texture_samplers,
                                                    texture_count);
}

/// @brief Apply supported KHR material extension factors to one runtime material.
/// @details Handles emissive strength, unlit shading, specular, clearcoat, transmission, IOR,
///          volume attenuation, sheen, and anisotropy. Extension textures without dedicated
///          runtime slots are still resolved and validated through the shared texture path.
/// @param mat_json glTF material object containing the extension map.
/// @param material Runtime material to update.
/// @param material_index Index used for per-slot metadata.
/// @param texture_images Resolved images indexed by texture.
/// @param texture_supported Supported-format flags indexed by texture.
/// @param texture_samplers Sampler descriptions indexed by texture.
/// @param texture_count Number of texture entries.
/// @param material_infos Per-material texture metadata table.
/// @return Accumulated hard-failure indicator from extension texture resolution.
static int gltf_material_apply_extensions(void *mat_json,
                                          void *material,
                                          int32_t material_index,
                                          void **texture_images,
                                          const uint8_t *texture_supported,
                                          const gltf_sampler_info_t *texture_samplers,
                                          int32_t texture_count,
                                          gltf_material_info_t *material_infos) {
    void *extensions = jget(mat_json, "extensions");
    void *emissive_strength =
        extensions ? jget(extensions, "KHR_materials_emissive_strength") : NULL;
    void *unlit = extensions ? jget(extensions, "KHR_materials_unlit") : NULL;
    void *specular = extensions ? jget(extensions, "KHR_materials_specular") : NULL;
    void *clearcoat = extensions ? jget(extensions, "KHR_materials_clearcoat") : NULL;
    void *transmission = extensions ? jget(extensions, "KHR_materials_transmission") : NULL;
    void *ior_ext = extensions ? jget(extensions, "KHR_materials_ior") : NULL;
    void *volume_ext = extensions ? jget(extensions, "KHR_materials_volume") : NULL;
    void *sheen_ext = extensions ? jget(extensions, "KHR_materials_sheen") : NULL;
    void *aniso_ext = extensions ? jget(extensions, "KHR_materials_anisotropy") : NULL;
    int load_failed = 0;
    if (emissive_strength)
        rt_material3d_set_emissive_intensity(
            material,
            gltf_clamp_double(jnum(emissive_strength, "emissiveStrength", 1.0), 0.0, DBL_MAX, 1.0));
    if (unlit) {
        rt_material3d_set_unlit(material, 1);
        rt_material3d_set_shading_model(material, 3);
    }
    load_failed |= gltf_material_apply_specular_extension(specular,
                                                          material,
                                                          material_index,
                                                          texture_images,
                                                          texture_supported,
                                                          texture_samplers,
                                                          texture_count,
                                                          material_infos);
    load_failed |= gltf_material_apply_clearcoat_extension(
        clearcoat, material, texture_images, texture_supported, texture_samplers, texture_count);
    load_failed |= gltf_material_apply_transmission_extension(
        transmission, material, texture_images, texture_supported, texture_samplers, texture_count);
    /* KHR_materials_ior: index of refraction drives the PBR branch's F0
     * (custom param 4; 0 = unset, shaders fall back to 1.5). */
    if (ior_ext) {
        double ior = gltf_clamp_double(jnum(ior_ext, "ior", 1.5), 1.0, 4.0, 1.5);
        rt_material3d_set_custom_param(material, 4, ior);
    }
    /* KHR_materials_volume: thickness (param 5) plus a folded Beer-Lambert
     * absorption coefficient (param 6) derived from attenuationColor and
     * attenuationDistance; the RGB attenuation folds to a per-channel-average
     * scalar so it rides the existing custom-parameter plumbing. */
    if (volume_ext) {
        double thickness =
            gltf_clamp_double(jnum(volume_ext, "thicknessFactor", 0.0), 0.0, DBL_MAX, 0.0);
        double att_distance =
            gltf_clamp_double(jnum(volume_ext, "attenuationDistance", 0.0), 0.0, DBL_MAX, 0.0);
        void *att_color = jarr(volume_ext, "attenuationColor");
        double sigma = 0.0;
        rt_material3d_set_custom_param(material, 5, thickness);
        if (att_distance > 0.0) {
            double avg = 1.0;
            if (jarr_len(att_color) >= 3) {
                double r =
                    gltf_clamp_double(jvalue_num(rt_seq_get(att_color, 0), 1.0), 1e-4, 1.0, 1.0);
                double g =
                    gltf_clamp_double(jvalue_num(rt_seq_get(att_color, 1), 1.0), 1e-4, 1.0, 1.0);
                double b =
                    gltf_clamp_double(jvalue_num(rt_seq_get(att_color, 2), 1.0), 1e-4, 1.0, 1.0);
                avg = (r + g + b) / 3.0;
            }
            sigma = -log(avg) / att_distance;
        }
        rt_material3d_set_custom_param(material, 6, sigma);
    }
    /* KHR_materials_sheen: the RGB sheen color folds to a luminance intensity
     * (param 0) with the roughness in param 7 — the Charlie lobe renders from
     * these on all four backends. Params 0/7 are safe inside the PBR gate
     * because Toon/Fresnel own params 0/1 only in their own shading models. */
    if (sheen_ext) {
        void *sheen_color = jarr(sheen_ext, "sheenColorFactor");
        double sheen_rough =
            gltf_clamp_double(jnum(sheen_ext, "sheenRoughnessFactor", 0.0), 0.0, 1.0, 0.0);
        double intensity = 0.0;
        if (jarr_len(sheen_color) >= 3) {
            double r =
                gltf_clamp_double(jvalue_num(rt_seq_get(sheen_color, 0), 0.0), 0.0, 1.0, 0.0);
            double g =
                gltf_clamp_double(jvalue_num(rt_seq_get(sheen_color, 1), 0.0), 0.0, 1.0, 0.0);
            double b =
                gltf_clamp_double(jvalue_num(rt_seq_get(sheen_color, 2), 0.0), 0.0, 1.0, 0.0);
            intensity = 0.2126 * r + 0.7152 * g + 0.0722 * b;
        }
        if (intensity > 0.0) {
            rt_material3d_set_custom_param(material, 0, intensity);
            rt_material3d_set_custom_param(material, 7, sheen_rough);
        }
    }
    /* KHR_materials_anisotropy: strength (param 8) and rotation in radians
     * (param 9) drive the anisotropic GGX lobe; the tangent frame comes from
     * the mesh tangents. The anisotropy texture is validated but not sampled
     * (same GL fragment-unit constraint as the clearcoat textures). */
    if (aniso_ext) {
        double strength =
            gltf_clamp_double(jnum(aniso_ext, "anisotropyStrength", 0.0), 0.0, 1.0, 0.0);
        double rotation = gltf_clamp_double(
            jnum(aniso_ext, "anisotropyRotation", 0.0), -6.2831853, 6.2831853, 0.0);
        if (strength > 0.0) {
            rt_material3d_set_custom_param(material, 8, strength);
            rt_material3d_set_custom_param(material, 9, rotation);
        }
    }
    return load_failed;
}

/// @brief Bind core normal, occlusion, and emissive textures and their scalar controls.
/// @param mat_json glTF material object.
/// @param material Runtime material to update.
/// @param material_index Index used for per-slot metadata.
/// @param texture_images Resolved images indexed by texture.
/// @param texture_supported Supported-format flags indexed by texture.
/// @param texture_samplers Sampler descriptions indexed by texture.
/// @param texture_count Number of texture entries.
/// @param material_infos Per-material texture metadata table.
/// @return Accumulated hard-failure indicator from texture resolution.
static int gltf_material_apply_standard_textures(void *mat_json,
                                                 void *material,
                                                 int32_t material_index,
                                                 void **texture_images,
                                                 const uint8_t *texture_supported,
                                                 const gltf_sampler_info_t *texture_samplers,
                                                 int32_t texture_count,
                                                 gltf_material_info_t *material_infos) {
    int load_failed = 0;
    void *normal_tex = jget(mat_json, "normalTexture");
    void *occlusion_tex = jget(mat_json, "occlusionTexture");
    void *emissive_tex = jget(mat_json, "emissiveTexture");
    load_failed |= gltf_material_bind_texture(
        normal_tex,
        material,
        RT_MATERIAL3D_TEXTURE_SLOT_NORMAL,
        texture_images,
        texture_supported,
        texture_samplers,
        texture_count,
        gltf_material_slot_info(material_infos, material_index, RT_MATERIAL3D_TEXTURE_SLOT_NORMAL),
        rt_material3d_set_normal_map);
    if (normal_tex)
        rt_material3d_set_normal_scale(material,
                                       gltf_finite_or(jnum(normal_tex, "scale", 1.0), 1.0));

    load_failed |= gltf_material_bind_texture(
        occlusion_tex,
        material,
        RT_MATERIAL3D_TEXTURE_SLOT_AO,
        texture_images,
        texture_supported,
        texture_samplers,
        texture_count,
        gltf_material_slot_info(material_infos, material_index, RT_MATERIAL3D_TEXTURE_SLOT_AO),
        rt_material3d_set_ao_map);
    if (occlusion_tex)
        rt_material3d_set_ao(
            material, gltf_clamp_double(jnum(occlusion_tex, "strength", 1.0), 0.0, 1.0, 1.0));

    load_failed |= gltf_material_bind_texture(
        emissive_tex,
        material,
        RT_MATERIAL3D_TEXTURE_SLOT_EMISSIVE,
        texture_images,
        texture_supported,
        texture_samplers,
        texture_count,
        gltf_material_slot_info(
            material_infos, material_index, RT_MATERIAL3D_TEXTURE_SLOT_EMISSIVE),
        rt_material3d_set_emissive_map);
    return load_failed;
}

/// @brief Apply glTF alpha-mode, cutoff, and double-sided rasterization state.
/// @param mat_json glTF material object.
/// @param material Runtime material to update.
static void gltf_material_apply_alpha_and_sided(void *mat_json, void *material) {
    const char *alpha_mode = jstr(mat_json, "alphaMode");
    if (alpha_mode && strcmp(alpha_mode, "MASK") == 0) {
        rt_material3d_set_alpha_mode(material, RT_MATERIAL3D_ALPHA_MODE_MASK);
        ((rt_material3d *)material)->alpha_cutoff =
            gltf_clamp_double(jnum(mat_json, "alphaCutoff", 0.5), 0.0, 1.0, 0.5);
    } else if (alpha_mode && strcmp(alpha_mode, "BLEND") == 0) {
        rt_material3d_set_alpha_mode(material, RT_MATERIAL3D_ALPHA_MODE_BLEND);
    } else {
        rt_material3d_set_alpha_mode(material, RT_MATERIAL3D_ALPHA_MODE_OPAQUE);
    }
    rt_material3d_set_double_sided(material, jbool(mat_json, "doubleSided", 0) ? 1 : 0);
}

/// @brief Resolve glTF materials (PBR factors, KHR material extensions, and the
///        normal/occlusion/emissive/specular texture slots) into Material3D
///        objects on the asset. Extracted post-asset phase of rt_gltf_load_impl;
///        never early-returns.
/// @note out_material_infos returns the per-material texture-slot metadata table
///       (heap, owned by the caller's cleanup). load_failed_io is read on entry
///       and set when a referenced texture has a missing-but-required payload.
/// @param asset Asset receiving the runtime material array.
/// @param root glTF JSON root containing material definitions.
/// @param texture_images Resolved images indexed by glTF texture.
/// @param texture_supported Supported-format flags indexed by texture.
/// @param texture_samplers Sampler descriptions indexed by texture.
/// @param texture_count Number of entries in the texture tables.
/// @param out_material_infos Receives the per-material texture-slot metadata table.
/// @param load_failed_io In/out accumulated hard-failure flag.
static void gltf_load_materials(rt_gltf_asset *asset,
                                void *root,
                                void **texture_images,
                                uint8_t *texture_supported,
                                gltf_sampler_info_t *texture_samplers,
                                int texture_count,
                                gltf_material_info_t **out_material_infos,
                                int *load_failed_io) {
    int load_failed = *load_failed_io;
    gltf_material_info_t *material_infos = NULL;
    // Extract materials
    void *mats_arr = jarr(root, "materials");
    int mat_count = 0;
    int material_capacity = 1;
    if (!gltf_jarr_len_i32(mats_arr, &mat_count) || mat_count > INT32_MAX - 1) {
        load_failed = 1;
        goto finish;
    }
    material_capacity = mat_count > 0 ? mat_count + 1 : 1;
    asset->materials = (void **)calloc((size_t)material_capacity, sizeof(void *));
    asset->material_capacity = asset->materials ? material_capacity : 0;
    material_infos =
        (gltf_material_info_t *)calloc((size_t)material_capacity, sizeof(gltf_material_info_t));
    if (!asset->materials || !material_infos) {
        asset->material_capacity = 0;
        load_failed = 1;
        goto finish;
    }
    if (material_infos) {
        for (int i = 0; i < material_capacity; i++)
            for (int slot = 0; slot < RT_MATERIAL3D_TEXTURE_SLOT_COUNT; slot++)
                gltf_texture_info_init(&material_infos[i].slots[slot]);
    }
    if (mat_count > 0 && asset->materials) {
        for (int i = 0; i < mat_count && asset->materials; i++) {
            void *mat_json = rt_seq_get(mats_arr, (int64_t)i);
            void *mat = NULL;
            void *pbr = jget(mat_json, "pbrMetallicRoughness");
            void *mat_extensions = jget(mat_json, "extensions");
            void *spec_gloss =
                mat_extensions ? jget(mat_extensions, "KHR_materials_pbrSpecularGlossiness") : NULL;

            if (spec_gloss) {
                /* Plan 09: legacy spec-gloss materials convert to metallic-roughness;
                 * the extension takes precedence over any fallback pbrMetallicRoughness. */
                mat = gltf_material_create_spec_gloss_from_json(spec_gloss);
                load_failed |= gltf_material_apply_spec_gloss_textures(spec_gloss,
                                                                       mat,
                                                                       i,
                                                                       texture_images,
                                                                       texture_supported,
                                                                       texture_samplers,
                                                                       texture_count,
                                                                       material_infos);
                gltf_material_synthesize_spec_gloss_mr_map(spec_gloss,
                                                           mat,
                                                           i,
                                                           texture_images,
                                                           texture_samplers,
                                                           texture_count,
                                                           material_infos);
            } else if (pbr) {
                mat = gltf_material_create_pbr_from_json(pbr);
                load_failed |= gltf_material_apply_pbr_textures(pbr,
                                                                mat,
                                                                i,
                                                                texture_images,
                                                                texture_supported,
                                                                texture_samplers,
                                                                texture_count,
                                                                material_infos);
            }
            if (!mat)
                mat = gltf_material_create_default_pbr();
            if (!mat)
                continue;

            gltf_material_apply_emissive_factor(mat_json, mat);
            load_failed |= gltf_material_apply_extensions(mat_json,
                                                          mat,
                                                          i,
                                                          texture_images,
                                                          texture_supported,
                                                          texture_samplers,
                                                          texture_count,
                                                          material_infos);
            load_failed |= gltf_material_apply_standard_textures(mat_json,
                                                                 mat,
                                                                 i,
                                                                 texture_images,
                                                                 texture_supported,
                                                                 texture_samplers,
                                                                 texture_count,
                                                                 material_infos);
            gltf_material_apply_alpha_and_sided(mat_json, mat);

            if (load_failed) {
                gltf_release_local(mat);
                break;
            }
            asset->materials[i] = mat;
            asset->material_count = i + 1;
        }
    }
finish:
    *out_material_infos = material_infos;
    *load_failed_io = load_failed;
}
