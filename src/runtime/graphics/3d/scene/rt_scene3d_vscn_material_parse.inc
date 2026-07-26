//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/scene/rt_scene3d_vscn_material_parse.inc
// Purpose: Parse VSCN Material3D values, sampler metadata, and resource references.
// Key invariants:
//   - Numeric render state is finite/range-sanitized before publication.
//   - Texture and cubemap indexes resolve only to complete staged table entries.
// Ownership/Lifetime:
//   - This implementation fragment is included by rt_scene3d_vscn_load.c.
//   - Parsed materials retain referenced runtime texture/cubemap handles through setters.
// Links: rt_scene3d_vscn_load.c, rt_scene3d_vscn_save.c, rt_canvas3d_internal.h
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements VSCN Material3D scalar, sampler, and resource-reference parsing.
/// @details This fragment is included after the loader's JSON accessors. It
///   sanitizes every published numeric/material mode and binds only validated
///   staged texture and cubemap table entries through retaining setters.

/// @brief Setter signature for one retained Material3D texture slot.
typedef void (*vscn_material_texture_setter_fn)(void *material, void *texture);

/// @brief Setter signature for one retained Material3D cubemap slot.
typedef void (*vscn_material_cubemap_setter_fn)(void *material, void *cubemap);

/// @brief Parse and clamp a four-component material color array when present.
/// @param material_obj Borrowed parsed JSON material object.
/// @param key Borrowed color-member name.
/// @param out_rgba In/out four-component destination/default.
/// @param hi Inclusive per-component upper bound.
static void vscn_parse_material_color4(void *material_obj,
                                       const char *key,
                                       double out_rgba[4],
                                       double hi) {
    void *arr = vjson_get(material_obj, key);
    if (arr && vjson_len(arr) >= 4) {
        out_rgba[0] = vscn_clamp_or(vjson_arr_f64(arr, 0, out_rgba[0]), out_rgba[0], 0.0, hi);
        out_rgba[1] = vscn_clamp_or(vjson_arr_f64(arr, 1, out_rgba[1]), out_rgba[1], 0.0, hi);
        out_rgba[2] = vscn_clamp_or(vjson_arr_f64(arr, 2, out_rgba[2]), out_rgba[2], 0.0, hi);
        out_rgba[3] = vscn_clamp_or(vjson_arr_f64(arr, 3, out_rgba[3]), out_rgba[3], 0.0, hi);
    }
}

/// @brief Parse and clamp a three-component material color array when present.
/// @param material_obj Borrowed parsed JSON material object.
/// @param key Borrowed color-member name.
/// @param out_rgb In/out three-component destination/default.
/// @param hi Inclusive per-component upper bound.
static void vscn_parse_material_color3(void *material_obj,
                                       const char *key,
                                       double out_rgb[3],
                                       double hi) {
    void *arr = vjson_get(material_obj, key);
    if (arr && vjson_len(arr) >= 3) {
        out_rgb[0] = vscn_clamp_or(vjson_arr_f64(arr, 0, out_rgb[0]), out_rgb[0], 0.0, hi);
        out_rgb[1] = vscn_clamp_or(vjson_arr_f64(arr, 1, out_rgb[1]), out_rgb[1], 0.0, hi);
        out_rgb[2] = vscn_clamp_or(vjson_arr_f64(arr, 2, out_rgb[2]), out_rgb[2], 0.0, hi);
    }
}

/// @brief Parse bounded scalar render state and material modes.
/// @param material_obj Borrowed parsed JSON material object.
/// @param material Borrowed mutable Material3D payload initialized with defaults.
static void vscn_parse_material_scalars(void *material_obj, rt_material3d *material) {
    material->shininess = vscn_nonnegative_or(
        vjson_f64(material_obj, "shininess", material->shininess), material->shininess);
    material->workflow = vscn_material_workflow_or(
        vjson_i64(material_obj, "workflow", material->workflow), material->workflow);
    material->alpha =
        vscn_clamp_or(vjson_f64(material_obj, "alpha", material->alpha), material->alpha, 0.0, 1.0);
    material->metallic = vscn_clamp_or(
        vjson_f64(material_obj, "metallic", material->metallic), material->metallic, 0.0, 1.0);
    material->roughness = vscn_clamp_or(
        vjson_f64(material_obj, "roughness", material->roughness), material->roughness, 0.0, 1.0);
    material->ao =
        vscn_clamp_or(vjson_f64(material_obj, "ao", material->ao), material->ao, 0.0, 1.0);
    material->emissive_intensity = vscn_nonnegative_or(
        vjson_f64(material_obj, "emissiveIntensity", material->emissive_intensity),
        material->emissive_intensity);
    material->normal_scale = vscn_nonnegative_or(
        vjson_f64(material_obj, "normalScale", material->normal_scale), material->normal_scale);
    material->alpha_mode = vscn_alpha_mode_or(
        vjson_i64(material_obj, "alphaMode", material->alpha_mode), material->alpha_mode);
    material->alpha_cutoff =
        vscn_clamp_or(vjson_f64(material_obj, "alphaCutoff", material->alpha_cutoff),
                      material->alpha_cutoff,
                      0.0,
                      1.0);
    material->double_sided =
        (int8_t)vjson_bool(material_obj, "doubleSided", material->double_sided);
    material->reflectivity =
        vscn_clamp_or(vjson_f64(material_obj, "reflectivity", material->reflectivity),
                      material->reflectivity,
                      0.0,
                      1.0);
    material->unlit = vjson_bool(material_obj, "unlit", material->unlit);
    material->shading_model =
        (int32_t)vjson_i64(material_obj, "shadingModel", material->shading_model);
    if (material->shading_model < 0 || material->shading_model > 5)
        material->shading_model = 0;
}

/// @brief Parse up to twelve sanitized custom shader parameters.
/// @param material_obj Borrowed parsed JSON material object.
/// @param material Borrowed mutable Material3D payload initialized with defaults.
static void vscn_parse_material_custom_params(void *material_obj, rt_material3d *material) {
    void *arr = vjson_get(material_obj, "customParams");
    for (int i = 0; i < 12; i++) {
        if (arr && i < vjson_len(arr))
            material->custom_params[i] = vscn_clamp_abs_or(
                vjson_arr_f64(arr, i, material->custom_params[i]), material->custom_params[i]);
    }
}

/// @brief Parse per-slot UV transforms, wrapping, filtering, and anisotropy.
/// @param material_obj Borrowed parsed JSON material object.
/// @param material Borrowed mutable Material3D payload initialized with defaults.
/// @return Nonzero when the optional slot array is absent or structurally valid.
static int vscn_parse_material_texture_slots(void *material_obj, rt_material3d *material) {
    void *arr = vjson_get(material_obj, "textureSlots");
    if (!arr)
        return 1;
    if (!vjson_is_seq(arr))
        return 0;
    for (int i = 0; i < RT_MATERIAL3D_TEXTURE_SLOT_COUNT && i < vjson_len(arr); i++) {
        void *slot_obj = rt_seq_get(arr, i);
        void *uv_arr = slot_obj ? vjson_get(slot_obj, "uvTransform") : NULL;
        if (!slot_obj || !vjson_is_map(slot_obj) || (uv_arr && !vjson_is_seq(uv_arr)))
            return 0;
        material->texture_slot_uv_set[i] =
            vjson_i64(slot_obj, "uvSet", material->texture_slot_uv_set[i]) > 0 ? 1 : 0;
        material->texture_slot_wrap_s[i] =
            vscn_wrap_or(vjson_i64(slot_obj, "wrapS", material->texture_slot_wrap_s[i]),
                         material->texture_slot_wrap_s[i]);
        material->texture_slot_wrap_t[i] =
            vscn_wrap_or(vjson_i64(slot_obj, "wrapT", material->texture_slot_wrap_t[i]),
                         material->texture_slot_wrap_t[i]);
        {
            int32_t legacy_filter =
                vscn_filter_or(vjson_i64(slot_obj, "filter", material->texture_slot_filter[i]),
                               material->texture_slot_filter[i]);
            material->texture_slot_min_filter[i] =
                vscn_filter_or(vjson_i64(slot_obj, "minFilter", legacy_filter), legacy_filter);
            material->texture_slot_mag_filter[i] =
                vscn_filter_or(vjson_i64(slot_obj, "magFilter", legacy_filter), legacy_filter);
            material->texture_slot_mip_filter[i] = vscn_mip_filter_or(
                vjson_i64(slot_obj, "mipFilter", RT_MATERIAL3D_TEXTURE_MIP_FILTER_NONE),
                RT_MATERIAL3D_TEXTURE_MIP_FILTER_NONE);
            {
                int64_t anisotropy =
                    vjson_i64(slot_obj, "anisotropy", material->texture_slot_anisotropy[i]);
                if (anisotropy < 1)
                    anisotropy = 1;
                if (anisotropy > 16)
                    anisotropy = 16;
                material->texture_slot_anisotropy[i] = (int32_t)anisotropy;
            }
            material->texture_slot_filter[i] = material->texture_slot_mag_filter[i];
        }
        for (int j = 0; j < 6; j++) {
            if (uv_arr && j < vjson_len(uv_arr))
                material->texture_slot_uv_transform[i][j] = vscn_clamp_abs_or(
                    vjson_arr_f64(uv_arr, j, material->texture_slot_uv_transform[i][j]),
                    material->texture_slot_uv_transform[i][j]);
        }
    }
    material->texture_wrap_s = material->texture_slot_wrap_s[RT_MATERIAL3D_TEXTURE_SLOT_BASE_COLOR];
    material->texture_wrap_t = material->texture_slot_wrap_t[RT_MATERIAL3D_TEXTURE_SLOT_BASE_COLOR];
    material->texture_filter = material->texture_slot_filter[RT_MATERIAL3D_TEXTURE_SLOT_BASE_COLOR];
    material->texture_min_filter =
        material->texture_slot_min_filter[RT_MATERIAL3D_TEXTURE_SLOT_BASE_COLOR];
    material->texture_mag_filter =
        material->texture_slot_mag_filter[RT_MATERIAL3D_TEXTURE_SLOT_BASE_COLOR];
    material->texture_mip_filter =
        material->texture_slot_mip_filter[RT_MATERIAL3D_TEXTURE_SLOT_BASE_COLOR];
    material->anisotropy = material->texture_slot_anisotropy[RT_MATERIAL3D_TEXTURE_SLOT_BASE_COLOR];
    return 1;
}

/// @brief Resolve one optional texture-table index and publish it through a retaining setter.
/// @param material_obj Borrowed parsed JSON material object.
/// @param key Borrowed texture-index member name.
/// @param material Borrowed destination Material3D payload.
/// @param textures Borrowed complete staged texture table.
/// @param tex_count Number of readable texture slots.
/// @param setter Non-NULL retaining setter for the destination material slot.
/// @return Nonzero for an absent or valid complete reference, otherwise zero.
static int vscn_bind_material_texture_ref(void *material_obj,
                                          const char *key,
                                          rt_material3d *material,
                                          void **textures,
                                          int tex_count,
                                          vscn_material_texture_setter_fn setter) {
    int64_t index;
    if (!vscn_read_index_ref(material_obj, key, &index) ||
        (index >= 0 && (index >= tex_count || !textures || !textures[index])))
        return 0;
    if (index >= 0 && index < tex_count && textures[index])
        setter(material, textures[index]);
    return 1;
}

/// @brief Resolve one optional cubemap-table index and publish it through a retaining setter.
/// @param material_obj Borrowed parsed JSON material object.
/// @param key Borrowed cubemap-index member name.
/// @param material Borrowed destination Material3D payload.
/// @param cubemaps Borrowed complete staged cubemap table.
/// @param cubemap_count Number of readable cubemap slots.
/// @param setter Non-NULL retaining setter for the destination material slot.
/// @return Nonzero for an absent or valid complete reference, otherwise zero.
static int vscn_bind_material_cubemap_ref(void *material_obj,
                                          const char *key,
                                          rt_material3d *material,
                                          rt_cubemap3d **cubemaps,
                                          int cubemap_count,
                                          vscn_material_cubemap_setter_fn setter) {
    int64_t index;
    if (!vscn_read_index_ref(material_obj, key, &index) ||
        (index >= 0 && (index >= cubemap_count || !cubemaps || !cubemaps[index])))
        return 0;
    if (index >= 0 && index < cubemap_count && cubemaps[index])
        setter(material, cubemaps[index]);
    return 1;
}

/// @brief Bind every serialized texture and environment-map reference on a material.
/// @param material_obj Borrowed parsed JSON material object.
/// @param material Borrowed destination Material3D payload.
/// @param textures Borrowed complete staged texture table.
/// @param tex_count Number of readable texture slots.
/// @param cubemaps Borrowed complete staged cubemap table.
/// @param cubemap_count Number of readable cubemap slots.
/// @return Nonzero only when every optional reference is absent or valid.
static int vscn_bind_material_refs(void *material_obj,
                                   rt_material3d *material,
                                   void **textures,
                                   int tex_count,
                                   rt_cubemap3d **cubemaps,
                                   int cubemap_count) {
    return vscn_bind_material_texture_ref(
               material_obj, "texture", material, textures, tex_count, rt_material3d_set_texture) &&
           vscn_bind_material_texture_ref(material_obj,
                                          "normalMap",
                                          material,
                                          textures,
                                          tex_count,
                                          rt_material3d_set_normal_map) &&
           vscn_bind_material_texture_ref(material_obj,
                                          "specularMap",
                                          material,
                                          textures,
                                          tex_count,
                                          rt_material3d_set_specular_map) &&
           vscn_bind_material_texture_ref(material_obj,
                                          "emissiveMap",
                                          material,
                                          textures,
                                          tex_count,
                                          rt_material3d_set_emissive_map) &&
           vscn_bind_material_texture_ref(material_obj,
                                          "metallicRoughnessMap",
                                          material,
                                          textures,
                                          tex_count,
                                          rt_material3d_set_metallic_roughness_map) &&
           vscn_bind_material_texture_ref(
               material_obj, "aoMap", material, textures, tex_count, rt_material3d_set_ao_map) &&
           vscn_bind_material_texture_ref(material_obj,
                                          "lightMap",
                                          material,
                                          textures,
                                          tex_count,
                                          rt_material3d_set_lightmap) &&
           vscn_bind_material_cubemap_ref(material_obj,
                                          "envMap",
                                          material,
                                          cubemaps,
                                          cubemap_count,
                                          rt_material3d_set_env_map);
}
