//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/backend/vgfx3d_backend_opengl_material.inc
// Purpose: OpenGL sampler/material/light binding, skybox draw, and uniform resolution.
// Key invariants:
//   - Every draw uploads an explicit particle/instancing mode and sanitized material snapshot.
//   - Invalid uniform locations are skipped without changing texture-unit ownership.
// Ownership/Lifetime:
//   - Uniform locations and samplers belong to gl_context_t for the linked program lifetime.
//   - Material textures are borrowed from the Canvas draw command during synchronous submission.
// Links: vgfx3d_backend_opengl.c, vgfx3d_backend_opengl_shaders.inc
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements OpenGL material, sampler, light, IBL, skybox, and uniform binding.
/// @details This fragment converts sanitized draw snapshots into program uniforms and texture
///          bindings, owns the per-context sampler cache, resolves specialized unlit programs,
///          and records uniform locations for all linked rendering passes.

/// @brief Activate texture unit `unit`, bind `texture`, and tell the program about it.
///
/// One-stop helper that handles the GL "select active unit, then
/// bind, then push the unit number to the sampler uniform" dance.
/// Skips the uniform write when the location is invalid (-1) — that
/// happens when the shader doesn't reference the sampler.
/// @param uniform_loc Sampler uniform location, or -1 to skip the uniform write.
/// @param unit Zero-based texture-unit index.
/// @param target Texture binding target.
/// @param texture OpenGL texture object name, or zero to unbind.
static void bind_texture_unit(GLint uniform_loc, int unit, GLenum target, GLuint texture) {
    gl.ActiveTexture(GL_TEXTURE0 + (GLenum)unit);
    gl.BindTexture(target, texture);
    gl.BindSampler((GLuint)unit, 0);
    if (uniform_loc >= 0)
        gl.Uniform1i(uniform_loc, unit);
}

/// @brief Map a Zanna material texture-wrap constant to the corresponding OpenGL enum.
/// @details RT_MATERIAL3D_TEXTURE_WRAP_CLAMP_TO_EDGE → GL_CLAMP_TO_EDGE,
///          RT_MATERIAL3D_TEXTURE_WRAP_MIRRORED_REPEAT → GL_MIRRORED_REPEAT,
///          all other values → GL_REPEAT (the GL default).
/// @param mode Runtime material wrap-mode constant.
/// @return The corresponding OpenGL wrap enum.
static GLenum gl_material_wrap_mode(int32_t mode) {
    if (mode == RT_MATERIAL3D_TEXTURE_WRAP_CLAMP_TO_EDGE)
        return GL_CLAMP_TO_EDGE;
    if (mode == RT_MATERIAL3D_TEXTURE_WRAP_MIRRORED_REPEAT)
        return GL_MIRRORED_REPEAT;
    return GL_REPEAT;
}

/// @brief Map a Zanna material texture-filter constant to the corresponding OpenGL enum.
/// @details RT_MATERIAL3D_TEXTURE_FILTER_NEAREST → GL_NEAREST (pixel-art / point sampling);
///          all other values → GL_LINEAR (bilinear).
/// @param mode Runtime material nearest/linear filter constant.
/// @return The corresponding OpenGL filtering enum.
static GLenum gl_material_filter_mode(int32_t mode) {
    return mode == RT_MATERIAL3D_TEXTURE_FILTER_NEAREST ? GL_NEAREST : GL_LINEAR;
}

/// @brief Map independent minification and mip axes to an OpenGL MIN_FILTER enum.
/// @param min_filter Nearest/linear texel filter.
/// @param mip_filter None/nearest/linear mip selection.
/// @return Exact OpenGL minification enum preserving both axes.
static GLenum gl_material_min_filter_mode(int32_t min_filter, int32_t mip_filter) {
    int nearest = min_filter == RT_MATERIAL3D_TEXTURE_FILTER_NEAREST;
    if (mip_filter == RT_MATERIAL3D_TEXTURE_MIP_FILTER_NEAREST)
        return nearest ? GL_NEAREST_MIPMAP_NEAREST : GL_LINEAR_MIPMAP_NEAREST;
    if (mip_filter == RT_MATERIAL3D_TEXTURE_MIP_FILTER_LINEAR)
        return nearest ? GL_NEAREST_MIPMAP_LINEAR : GL_LINEAR_MIPMAP_LINEAR;
    return nearest ? GL_NEAREST : GL_LINEAR;
}

/// @brief Pack independent min, mag, and mip axes into the sampler-cache filter index.
/// @param min_filter Sanitized nearest/linear minification filter.
/// @param mag_filter Sanitized nearest/linear magnification filter.
/// @param mip_filter Sanitized none/nearest/linear mip selector.
/// @return Stable index in `[0, 12)`.
static int gl_sampler_filter_index(int32_t min_filter, int32_t mag_filter, int32_t mip_filter) {
    int min_index = min_filter == RT_MATERIAL3D_TEXTURE_FILTER_NEAREST ? 1 : 0;
    int mag_index = mag_filter == RT_MATERIAL3D_TEXTURE_FILTER_NEAREST ? 1 : 0;
    int mip_index = mip_filter == RT_MATERIAL3D_TEXTURE_MIP_FILTER_NEAREST
                        ? 1
                        : (mip_filter == RT_MATERIAL3D_TEXTURE_MIP_FILTER_LINEAR ? 2 : 0);
    return min_index + mag_index * 2 + mip_index * 4;
}

/// @brief Map a material texture-wrap mode constant to a 3-entry sampler-cache index.
/// @details The material sampler cache is keyed by wrap_s, wrap_t, filter, and anisotropy; each
///   wrap axis has exactly 3 valid values. This function maps the abstract
///   `RT_MATERIAL3D_TEXTURE_WRAP_*` constant to the corresponding compact array index:
///     - 0 = GL_REPEAT (the default)
///     - 1 = GL_CLAMP_TO_EDGE
///     - 2 = GL_MIRRORED_REPEAT
///   Any unrecognised value falls through to 0 (repeat), matching the GL default.
/// @param mode  One of the `RT_MATERIAL3D_TEXTURE_WRAP_*` constants.
/// @return Array index in [0, 2] for use as a `material_samplers` axis subscript.
static int gl_sampler_wrap_index(int32_t mode) {
    if (mode == RT_MATERIAL3D_TEXTURE_WRAP_CLAMP_TO_EDGE)
        return 1;
    if (mode == RT_MATERIAL3D_TEXTURE_WRAP_MIRRORED_REPEAT)
        return 2;
    return 0;
}

/// @brief Resolve the anisotropy value actually usable by this GL context.
/// @param ctx Borrowed OpenGL context containing extension support and the device maximum.
/// @param requested Requested material anisotropy.
/// @param filter Effective nearest/linear filter selection.
/// @return A supported anisotropy value of at least one.
static int32_t gl_effective_sampler_anisotropy(gl_context_t *ctx,
                                               int32_t requested,
                                               int32_t filter) {
    int32_t anisotropy;
    if (!ctx || filter == RT_MATERIAL3D_TEXTURE_FILTER_NEAREST || !ctx->anisotropy_supported ||
        ctx->max_texture_anisotropy <= 1.0f)
        return 1;
    anisotropy = vgfx3d_opengl_sanitize_anisotropy(requested);
    if (anisotropy > (int32_t)ctx->max_texture_anisotropy)
        anisotropy = (int32_t)ctx->max_texture_anisotropy;
    return anisotropy < 1 ? 1 : anisotropy;
}

/// @brief Return a cached or newly-created OpenGL sampler object for the given draw command and
/// texture slot.
/// @details Resolves wrap_s, wrap_t, filter, and anisotropy from the per-slot or primary material
///          parameters, maps them to cache indices via `gl_sampler_wrap_index`, and returns the
///          cached sampler
///          when available. Otherwise generates a new GL sampler, configures WRAP_S/T and
///          MIN/MAG filters, stores it in the material sampler cache, and returns it. Returns 0 on
///          null context/cmd or GL sampler generation failure.
/// @param ctx Borrowed OpenGL context owning the sampler cache.
/// @param cmd Borrowed draw command containing global and per-slot sampler state.
/// @param slot Texture slot to resolve, or a negative value for global state.
/// @return A configured cached sampler object name, or zero on failure.
static GLuint gl_get_material_sampler(gl_context_t *ctx,
                                      const vgfx3d_draw_cmd_t *cmd,
                                      int32_t slot) {
    int use_slot = cmd && slot >= 0 && slot < RT_MATERIAL3D_TEXTURE_SLOT_COUNT;
    int wrap_s;
    int wrap_t;
    int32_t min_filter;
    int32_t mag_filter;
    int32_t mip_filter;
    int filter_index;
    int32_t anisotropy;
    int32_t anisotropy_index;
    GLenum gl_min_filter;
    GLenum gl_mag_filter;
    GLuint sampler;
    if (!ctx || !cmd)
        return 0;
    wrap_s = gl_sampler_wrap_index(use_slot ? cmd->texture_slot_wrap_s[slot] : cmd->texture_wrap_s);
    wrap_t = gl_sampler_wrap_index(use_slot ? cmd->texture_slot_wrap_t[slot] : cmd->texture_wrap_t);
    min_filter = use_slot ? cmd->texture_slot_min_filter[slot] : cmd->texture_min_filter;
    mag_filter = use_slot ? cmd->texture_slot_mag_filter[slot] : cmd->texture_mag_filter;
    mip_filter = use_slot ? cmd->texture_slot_mip_filter[slot] : cmd->texture_mip_filter;
    min_filter = min_filter == RT_MATERIAL3D_TEXTURE_FILTER_NEAREST
                     ? RT_MATERIAL3D_TEXTURE_FILTER_NEAREST
                     : RT_MATERIAL3D_TEXTURE_FILTER_LINEAR;
    mag_filter = mag_filter == RT_MATERIAL3D_TEXTURE_FILTER_NEAREST
                     ? RT_MATERIAL3D_TEXTURE_FILTER_NEAREST
                     : RT_MATERIAL3D_TEXTURE_FILTER_LINEAR;
    mip_filter = mip_filter == RT_MATERIAL3D_TEXTURE_MIP_FILTER_NEAREST
                     ? RT_MATERIAL3D_TEXTURE_MIP_FILTER_NEAREST
                     : (mip_filter == RT_MATERIAL3D_TEXTURE_MIP_FILTER_LINEAR
                            ? RT_MATERIAL3D_TEXTURE_MIP_FILTER_LINEAR
                            : RT_MATERIAL3D_TEXTURE_MIP_FILTER_NONE);
    anisotropy = gl_effective_sampler_anisotropy(
        ctx,
        use_slot ? cmd->texture_slot_anisotropy[slot] : cmd->texture_anisotropy,
        (min_filter == RT_MATERIAL3D_TEXTURE_FILTER_NEAREST ||
         mag_filter == RT_MATERIAL3D_TEXTURE_FILTER_NEAREST)
            ? RT_MATERIAL3D_TEXTURE_FILTER_NEAREST
            : RT_MATERIAL3D_TEXTURE_FILTER_LINEAR);
    filter_index = gl_sampler_filter_index(min_filter, mag_filter, mip_filter);
    anisotropy_index = vgfx3d_opengl_sampler_anisotropy_index(anisotropy);
    sampler = ctx->material_samplers[wrap_s][wrap_t][filter_index][anisotropy_index];
    if (sampler)
        return sampler;
    gl.GenSamplers(1, &sampler);
    if (!sampler)
        return 0;
    gl_min_filter = gl_material_min_filter_mode(min_filter, mip_filter);
    gl_mag_filter = gl_material_filter_mode(mag_filter);
    gl.SamplerParameteri(sampler,
                         GL_TEXTURE_WRAP_S,
                         (GLint)gl_material_wrap_mode(use_slot ? cmd->texture_slot_wrap_s[slot]
                                                               : cmd->texture_wrap_s));
    gl.SamplerParameteri(sampler,
                         GL_TEXTURE_WRAP_T,
                         (GLint)gl_material_wrap_mode(use_slot ? cmd->texture_slot_wrap_t[slot]
                                                               : cmd->texture_wrap_t));
    /*
     * Generic material samplers must not require mip chains: runtime Pixels
     * textures and streaming fallbacks may expose only mip level 0. Native
     * texture uploads still configure mipmapped filtering when a full mip chain
     * is present.
     */
    gl.SamplerParameteri(sampler, GL_TEXTURE_MIN_FILTER, (GLint)gl_min_filter);
    gl.SamplerParameteri(sampler, GL_TEXTURE_MAG_FILTER, (GLint)gl_mag_filter);
    if (anisotropy > 1)
        gl.SamplerParameterf(sampler, GL_TEXTURE_MAX_ANISOTROPY_EXT, (GLfloat)anisotropy);
    if (!gl_upload_ok()) {
        gl.DeleteSamplers(1, &sampler);
        return 0;
    }
    ctx->material_samplers[wrap_s][wrap_t][filter_index][anisotropy_index] = sampler;
    return sampler;
}

/// @brief Bind a material texture to a unit and apply the draw command's sampler state.
/// @details Extends `bind_texture_unit` with per-material wrap and filter state. For 2D
///   textures with a valid draw command, it first attempts to retrieve a pre-built
///   `GLuint` sampler object from the `material_samplers` cache via `gl_get_material_sampler`.
///   If a cached sampler exists it is bound via `glBindSampler` (DSA-style), which overrides
///   any texture-object state. If no cached sampler is available (i.e., the sampler object
///   could not be created), the wrap and filter parameters are applied directly to the texture
///   object via `glTexParameteri` as a fallback. For non-2D targets (cubemaps etc.) the
///   call degrades to a plain `bind_texture_unit` with no sampler override.
///   @p slot selects which per-slot texture parameters to use from the draw command when it
///   is in [0, RT_MATERIAL3D_TEXTURE_SLOT_COUNT); a negative slot uses the command's global
///   wrap/filter fields instead.
/// @param ctx          Active OpenGL backend context (holds the sampler object cache).
/// @param uniform_loc  GLSL sampler uniform location; -1 is safe (write is skipped).
/// @param unit         Texture unit index (0-based); activates `GL_TEXTURE0 + unit`.
/// @param target       Texture target (e.g. `GL_TEXTURE_2D`, `GL_TEXTURE_CUBE_MAP`).
/// @param texture      GL texture name to bind; 0 effectively unbinds.
/// @param cmd          Draw command providing wrap/filter parameters; may be NULL.
/// @param slot         Per-slot index for texture parameters, or -1 for global fields.
static void bind_texture_unit_with_sampler(gl_context_t *ctx,
                                           GLint uniform_loc,
                                           int unit,
                                           GLenum target,
                                           GLuint texture,
                                           const vgfx3d_draw_cmd_t *cmd,
                                           int32_t slot) {
    bind_texture_unit(uniform_loc, unit, target, texture);
    if (texture != 0 && target == GL_TEXTURE_2D && cmd) {
        int use_slot = slot >= 0 && slot < RT_MATERIAL3D_TEXTURE_SLOT_COUNT;
        int32_t min_filter =
            use_slot ? cmd->texture_slot_min_filter[slot] : cmd->texture_min_filter;
        int32_t mag_filter =
            use_slot ? cmd->texture_slot_mag_filter[slot] : cmd->texture_mag_filter;
        int32_t mip_filter =
            use_slot ? cmd->texture_slot_mip_filter[slot] : cmd->texture_mip_filter;
        GLenum gl_min_filter = gl_material_min_filter_mode(min_filter, mip_filter);
        GLenum gl_mag_filter = gl_material_filter_mode(mag_filter);
        int32_t anisotropy = gl_effective_sampler_anisotropy(
            ctx,
            use_slot ? cmd->texture_slot_anisotropy[slot] : cmd->texture_anisotropy,
            (min_filter == RT_MATERIAL3D_TEXTURE_FILTER_NEAREST ||
             mag_filter == RT_MATERIAL3D_TEXTURE_FILTER_NEAREST)
                ? RT_MATERIAL3D_TEXTURE_FILTER_NEAREST
                : RT_MATERIAL3D_TEXTURE_FILTER_LINEAR);
        GLuint sampler = gl_get_material_sampler(ctx, cmd, slot);
        if (sampler) {
            gl.BindSampler((GLuint)unit, sampler);
            return;
        }
        gl.TexParameteri(target,
                         GL_TEXTURE_WRAP_S,
                         (GLint)gl_material_wrap_mode(use_slot ? cmd->texture_slot_wrap_s[slot]
                                                               : cmd->texture_wrap_s));
        gl.TexParameteri(target,
                         GL_TEXTURE_WRAP_T,
                         (GLint)gl_material_wrap_mode(use_slot ? cmd->texture_slot_wrap_t[slot]
                                                               : cmd->texture_wrap_t));
        gl.TexParameteri(target, GL_TEXTURE_MIN_FILTER, (GLint)gl_min_filter);
        gl.TexParameteri(target, GL_TEXTURE_MAG_FILTER, (GLint)gl_mag_filter);
        if (anisotropy > 1)
            gl.TexParameterf(target, GL_TEXTURE_MAX_ANISOTROPY_EXT, (GLfloat)anisotropy);
    }
}

/// @brief Push up to `VGFX3D_MAX_LIGHTS` lights' uniforms to the program.
///
/// Uniforms are arrays-of-uniforms (one location per light index)
/// rather than a UBO — simpler to set up and small enough not to
/// matter performance-wise.
/// @param ctx Borrowed OpenGL context containing the active program's light locations.
/// @param lights Borrowed array of light snapshots, or NULL.
/// @param light_count Number of entries available through @p lights.
static void upload_light_uniforms(gl_context_t *ctx,
                                  const vgfx3d_light_params_t *lights,
                                  int32_t light_count) {
    if (!lights || light_count < 0)
        light_count = 0;
    if (light_count > VGFX3D_MAX_LIGHTS)
        light_count = VGFX3D_MAX_LIGHTS;
    gl.Uniform1i(ctx->uLightCount, light_count);
    for (int32_t i = 0; i < light_count; i++) {
        int32_t shadow_index = gl_sanitize_shadow_slot(ctx, lights[i].shadow_index);
        int32_t native_emitter = lights[i].type >= 4 && lights[i].type <= 6;
        gl.Uniform1i(ctx->uLightType[i], lights[i].type);
        gl.Uniform1i(ctx->uLightShadowIndex[i], shadow_index);
        gl.Uniform1i(ctx->uLightShadowCascadeCount[i],
                     native_emitter ? lights[i].decay_type
                                    : (shadow_index >= 0 ? lights[i].shadow_cascade_count : 1));
        gl.Uniform1i(ctx->uLightShadowProjectionType[i],
                     shadow_index >= 0 ? lights[i].shadow_projection_type
                                       : VGFX3D_SHADOW_PROJECTION_ORTHOGRAPHIC);
        if (native_emitter) {
            /* Native emitters never use cascaded shadows. Pack their U basis/width into the
             * otherwise-unused cascade vector and height-or-radius/range into the cone scalars.
             * This preserves the GL 3.3 sampler/uniform baseline without losing emitter state. */
            gl.Uniform4f(ctx->uLightShadowCascadeSplits[i],
                         lights[i].basis_u[0],
                         lights[i].basis_u[1],
                         lights[i].basis_u[2],
                         lights[i].width);
        } else {
            gl.Uniform4f(ctx->uLightShadowCascadeSplits[i],
                         lights[i].shadow_cascade_splits[0],
                         lights[i].shadow_cascade_splits[1],
                         lights[i].shadow_cascade_splits[2],
                         lights[i].shadow_cascade_splits[3]);
        }
        gl.Uniform3f(ctx->uLightDir[i],
                     lights[i].direction[0],
                     lights[i].direction[1],
                     lights[i].direction[2]);
        gl.Uniform3f(
            ctx->uLightPos[i], lights[i].position[0], lights[i].position[1], lights[i].position[2]);
        gl.Uniform3f(
            ctx->uLightColor[i], lights[i].color[0], lights[i].color[1], lights[i].color[2]);
        gl.Uniform1f(ctx->uLightIntensity[i], lights[i].intensity);
        gl.Uniform1f(ctx->uLightAtten[i], lights[i].attenuation);
        gl.Uniform1f(ctx->uLightInnerCos[i],
                     native_emitter ? (lights[i].type == 4 ? lights[i].height : lights[i].radius)
                                    : lights[i].inner_cos);
        gl.Uniform1f(ctx->uLightOuterCos[i],
                     native_emitter ? lights[i].range : lights[i].outer_cos);
    }
}

/// @brief Upload one CPU froxel table into the cluster UBOs (or select the flat loop).
/// @details The u16 offset/index arrays are little-endian, which is exactly the
///          std140 uvec4 lane layout the shader unpacks (two u16 per uint lane),
///          so the tables upload as-is with no repacking pass.
/// @param ctx Borrowed OpenGL context owning the cluster UBOs and selector uniform.
/// @param table Borrowed CPU cluster table to validate and upload.
/// @param expected_revision Light revision that @p table must describe.
/// @param light_count Number of lights expected by the draw.
static void gl_upload_cluster_table(gl_context_t *ctx,
                                    const vgfx3d_cluster_table_t *table,
                                    uint32_t expected_revision,
                                    int32_t light_count) {
    if (!ctx || !vgfx3d_cluster_table_is_usable(table, expected_revision, light_count) ||
        !ctx->cluster_offsets_ubo || !ctx->cluster_indices_ubo) {
        if (ctx)
            gl.Uniform1i(ctx->uClusterGlobalCount, -1);
        return;
    }
    gl.Uniform1i(ctx->uClusterGlobalCount, table->global_light_count);
    gl.BindBuffer(GL_UNIFORM_BUFFER, ctx->cluster_offsets_ubo);
    gl.BufferSubData(GL_UNIFORM_BUFFER, 0, (GLsizeiptr)sizeof(table->offsets), table->offsets);
    gl.BindBuffer(GL_UNIFORM_BUFFER, ctx->cluster_indices_ubo);
    gl.BufferSubData(GL_UNIFORM_BUFFER, 0, (GLsizeiptr)sizeof(table->indices), table->indices);
    gl.BindBuffer(GL_UNIFORM_BUFFER, 0);
}

/// @brief Resolve a fully resident, structurally valid IBL tail for one draw.
/// @details The runtime metadata can become ready before a budgeted GPU overlay has completed.
/// Enabling IBL from metadata alone makes the shader sample ordinary box-filtered base mips (or
/// even an invalid LOD span). Require the current cubemap generation and IBL identity to be
/// present in the native cache before publishing the tail to the shader.
/// @param ctx Borrowed OpenGL context containing IBL state and cubemap cache entries.
/// @param cmd Borrowed draw command whose environment-map request is resolved.
/// @param[out] out_lod_base Required destination for the first prefiltered mip level.
/// @param[out] out_max_lod Required destination for the effective IBL LOD range.
/// @return 1 when a complete matching IBL tail is resident, otherwise 0.
static int gl_cmd_ibl_draw(gl_context_t *ctx,
                           const vgfx3d_draw_cmd_t *cmd,
                           float *out_lod_base,
                           float *out_max_lod) {
    const rt_cubemap3d *env_cm;
    int32_t level_base;
    uint64_t generation;

    if (out_lod_base)
        *out_lod_base = 0.0f;
    if (out_max_lod)
        *out_max_lod = 0.0f;
    if (!ctx || !cmd || !out_lod_base || !out_max_lod)
        return 0;
    env_cm = (const rt_cubemap3d *)cmd->env_map;
    *out_max_lod = env_cm ? gl_cubemap_max_lod(env_cm) : 0.0f;
    if (!ctx->ibl_enabled || !cmd->ibl_env || !env_cm || !env_cm->ibl_ready ||
        env_cm->ibl_identity == 0 || env_cm->face_size <= 0 || env_cm->face_size > INT32_MAX ||
        env_cm->ibl_base_size <= 0 || env_cm->ibl_base_size > INT32_MAX ||
        env_cm->ibl_mip_count <= 0 || env_cm->ibl_mip_count > INT32_MAX ||
        !vgfx3d_validate_cubemap_ibl_layout((int32_t)env_cm->face_size,
                                            (int32_t)env_cm->ibl_base_size,
                                            (int32_t)env_cm->ibl_mip_count,
                                            RT_CUBEMAP3D_IBL_MAX_MIPS,
                                            &level_base)) {
        return 0;
    }

    generation = vgfx3d_get_cubemap_generation(env_cm);
    for (int32_t i = 0; i < ctx->cubemap_cache_count; i++) {
        const gl_cubemap_cache_entry_t *entry = &ctx->cubemap_cache[i];
        if (entry->cubemap == env_cm && entry->generation == generation && entry->tex &&
            !entry->upload_in_progress && entry->applied_ibl_identity == env_cm->ibl_identity) {
            *out_lod_base = (float)level_base;
            *out_max_lod = (float)(env_cm->ibl_mip_count - 1);
            return 1;
        }
    }
    return 0;
}

/// @brief Push every per-draw uniform required by the active main or specialized program.
/// @details Uploads matrices, camera/material state, lighting, fog, shadows, IBL, clustering,
///          animation flags, and morph payload bindings for a sanitized draw command.
/// @param ctx Borrowed OpenGL context containing the active program and uniform locations.
/// @param cmd Borrowed sanitized draw command.
/// @param lights Borrowed sanitized light array, or NULL.
/// @param light_count Number of entries available through @p lights.
/// @param ambient Borrowed three-component ambient color, or NULL for black.
/// @param instanced Nonzero when matrix instance attributes are active.
static void upload_main_uniforms(gl_context_t *ctx,
                                 const vgfx3d_draw_cmd_t *cmd,
                                 const vgfx3d_light_params_t *lights,
                                 int32_t light_count,
                                 const float *ambient,
                                 int8_t instanced) {
    float model_matrix[16];
    float prev_model_matrix[16];
    float normal_matrix[16];
    static const float zero_ambient[3] = {0.0f, 0.0f, 0.0f};
    if (!ambient)
        ambient = zero_ambient;
    vgfx3d_copy_mat4_finite_or_identity(model_matrix, cmd->model_matrix);
    vgfx3d_copy_mat4_finite_or(prev_model_matrix,
                               cmd->has_prev_model_matrix ? cmd->prev_model_matrix
                                                          : cmd->model_matrix,
                               model_matrix);
    vgfx3d_compute_normal_matrix4(model_matrix, normal_matrix);

    gl.UniformMatrix4fv(ctx->uModelMatrix, 1, GL_TRUE, model_matrix);
    gl.UniformMatrix4fv(ctx->uPrevModelMatrix, 1, GL_TRUE, prev_model_matrix);
    gl.UniformMatrix4fv(ctx->uViewProjection, 1, GL_TRUE, ctx->vp);
    gl.UniformMatrix4fv(ctx->uPrevViewProjection, 1, GL_TRUE, ctx->draw_prev_vp);
    gl.UniformMatrix4fv(ctx->uNormalMatrix, 1, GL_TRUE, normal_matrix);
    gl.Uniform1i(ctx->uShadowCount, ctx->shadow_count);
    for (int32_t slot = 0; slot < VGFX3D_MAX_SHADOW_LIGHTS; slot++)
        gl.UniformMatrix4fv(ctx->uShadowVP[slot], 1, GL_TRUE, ctx->shadow_vp[slot]);

    gl.Uniform3f(ctx->uCameraPos, ctx->cam_pos[0], ctx->cam_pos[1], ctx->cam_pos[2]);
    gl.Uniform3f(
        ctx->uCameraForward, ctx->cam_forward[0], ctx->cam_forward[1], ctx->cam_forward[2]);
    gl.Uniform3f(ctx->uAmbientColor, ambient[0], ambient[1], ambient[2]);
    gl.Uniform4f(ctx->uDiffuseColor,
                 cmd->diffuse_color[0],
                 cmd->diffuse_color[1],
                 cmd->diffuse_color[2],
                 cmd->diffuse_color[3]);
    gl.Uniform4f(
        ctx->uSpecularColor, cmd->specular[0], cmd->specular[1], cmd->specular[2], cmd->shininess);
    gl.Uniform3f(ctx->uEmissiveColor,
                 cmd->emissive_color[0],
                 cmd->emissive_color[1],
                 cmd->emissive_color[2]);
    gl.Uniform4f(
        ctx->uPbrScalars0, cmd->metallic, cmd->roughness, cmd->ao, cmd->emissive_intensity);
    gl.Uniform4f(ctx->uPbrScalars1,
                 cmd->normal_scale,
                 cmd->alpha_cutoff,
                 (ctx->opaque_depth_valid && !cmd->has_splat) ? cmd->soft_particle_fade : 0.0f,
                 vgfx3d_draw_cmd_ssr_mask(cmd));
    /* Plan 10: viewport + clip planes uploaded per draw (cluster lookup and
     * depth linearization both read them, clustered or not). */
    gl.Uniform4f(ctx->uClusterParams,
                 (float)(ctx->rtt_active ? ctx->rtt_width : ctx->width),
                 (float)(ctx->rtt_active ? ctx->rtt_height : ctx->height),
                 ctx->cam_znear,
                 ctx->cam_zfar);
    {
        float env_lod_base = 0.0f;
        float env_max_lod = 0.0f;
        int ibl_draw = gl_cmd_ibl_draw(ctx, cmd, &env_lod_base, &env_max_lod);
        gl.Uniform1i(ctx->uHasEnvMap,
                     (cmd->env_map && (cmd->reflectivity > 0.0001f || ibl_draw)) ? 1 : 0);
        gl.Uniform1f(ctx->uReflectivity, cmd->reflectivity);
        gl.Uniform1f(ctx->uEnvMaxLod, env_max_lod);
        gl.Uniform1f(ctx->uEnvLodBase, env_lod_base);
        gl.Uniform1i(ctx->uIblEnabled, ibl_draw);
        gl.Uniform1f(ctx->uIblIntensity, ctx->ibl_intensity);
        gl.Uniform3fv(ctx->uShIrradiance, 9, ctx->ibl_sh);
    }
    gl.Uniform1f(ctx->uAlpha, cmd->alpha);
    gl.Uniform1i(ctx->uUnlit, cmd->unlit);
    gl.Uniform1i(ctx->uShadingModel, vgfx3d_sanitize_shading_model(cmd->shading_model));
    gl.Uniform1i(ctx->uWorkflow, vgfx3d_sanitize_material_workflow(cmd->workflow));
    gl.Uniform1i(ctx->uAlphaMode, vgfx3d_sanitize_alpha_mode(cmd->alpha_mode));
    gl.Uniform4i(ctx->uTextureUvSets0,
                 vgfx3d_sanitize_texture_uv_set(cmd->texture_slot_uv_set[0]),
                 vgfx3d_sanitize_texture_uv_set(cmd->texture_slot_uv_set[1]),
                 vgfx3d_sanitize_texture_uv_set(cmd->texture_slot_uv_set[2]),
                 vgfx3d_sanitize_texture_uv_set(cmd->texture_slot_uv_set[3]));
    gl.Uniform4i(ctx->uTextureUvSets1,
                 vgfx3d_sanitize_texture_uv_set(cmd->texture_slot_uv_set[4]),
                 vgfx3d_sanitize_texture_uv_set(cmd->texture_slot_uv_set[5]),
                 0,
                 0);
    {
        float uv_transform0[RT_MATERIAL3D_TEXTURE_SLOT_COUNT][4];
        float uv_transform1[RT_MATERIAL3D_TEXTURE_SLOT_COUNT][4];
        for (int32_t slot = 0; slot < RT_MATERIAL3D_TEXTURE_SLOT_COUNT; slot++) {
            uv_transform0[slot][0] = cmd->texture_slot_uv_transform[slot][0];
            uv_transform0[slot][1] = cmd->texture_slot_uv_transform[slot][1];
            uv_transform0[slot][2] = cmd->texture_slot_uv_transform[slot][2];
            uv_transform0[slot][3] = cmd->texture_slot_uv_transform[slot][3];
            uv_transform1[slot][0] = cmd->texture_slot_uv_transform[slot][4];
            uv_transform1[slot][1] = cmd->texture_slot_uv_transform[slot][5];
            uv_transform1[slot][2] = 0.0f;
            uv_transform1[slot][3] = 0.0f;
        }
        gl.Uniform4fv(
            ctx->uTextureUvTransform0, RT_MATERIAL3D_TEXTURE_SLOT_COUNT, &uv_transform0[0][0]);
        gl.Uniform4fv(
            ctx->uTextureUvTransform1, RT_MATERIAL3D_TEXTURE_SLOT_COUNT, &uv_transform1[0][0]);
    }
    gl.Uniform1i(ctx->uCameraIsOrtho, ctx->cam_is_ortho ? 1 : 0);
    gl.Uniform1fv(ctx->uCustomParams, 12, cmd->custom_params);
    gl.Uniform1i(ctx->uUseInstancing, instanced ? 1 : 0);
    gl.Uniform1i(ctx->uParticleMode, cmd->particle_instances ? 1 : 0);
    gl.Uniform1i(ctx->uHasPrevModelMatrix, cmd->has_prev_model_matrix ? 1 : 0);
    gl.Uniform1i(ctx->uHasPrevInstanceMatrices, cmd->has_prev_instance_matrices ? 1 : 0);
    gl.Uniform1i(ctx->uHasPrevSkinning, cmd->prev_bone_palette ? 1 : 0);
    gl.Uniform1i(ctx->uHasPrevMorphWeights, cmd->prev_morph_weights ? 1 : 0);
    gl.Uniform1i(ctx->uFogEnabled,
                 (ctx->fog_enabled && ctx->cam_is_ortho && !cmd->disable_depth_test) ? 1 : 0);
    gl.Uniform1f(ctx->uFogNear, ctx->fog_near);
    gl.Uniform4f(ctx->uHeightFog,
                 ctx->height_fog[0],
                 ctx->height_fog[1],
                 ctx->height_fog[2],
                 ctx->height_fog[3]);
    gl.Uniform4f(ctx->uHeightFogSun,
                 ctx->height_fog_sun[0],
                 ctx->height_fog_sun[1],
                 ctx->height_fog_sun[2],
                 ctx->height_fog_sun[3]);
    gl.Uniform4f(ctx->uHeightFogSunDir,
                 ctx->height_fog_sun_dir[0],
                 ctx->height_fog_sun_dir[1],
                 ctx->height_fog_sun_dir[2],
                 ctx->height_fog_sun_dir[3]);
    gl.Uniform1f(ctx->uFogFar, ctx->fog_far);
    gl.Uniform3f(ctx->uFogColor, ctx->fog_color[0], ctx->fog_color[1], ctx->fog_color[2]);
    gl.Uniform1f(ctx->uShadowBias, ctx->shadow_bias);
    gl.Uniform1f(ctx->uShadowSlopeBias, ctx->shadow_slope_bias);
    gl.Uniform1f(ctx->uShadowStrength, ctx->shadow_strength);
    /* Quality tier → rotated-Poisson tap count (0=4, 1=8, 2=16). */
    gl.Uniform1i(ctx->uShadowSampleCount,
                 ctx->shadow_quality <= 0 ? 4 : (ctx->shadow_quality == 1 ? 8 : 16));

    /* GL uniform values persist in the program object, so runs of draws that
     * share a light-snapshot revision skip the 64-light glUniform storm. */
    if (cmd->lights_revision == 0 || cmd->lights_revision != ctx->uploaded_lights_revision) {
        upload_light_uniforms(ctx, lights, light_count);
        ctx->uploaded_lights_revision = cmd->lights_revision;
    }
    if (cmd->lights_revision == 0 || cmd->lights_revision != ctx->uploaded_cluster_revision) {
        gl_upload_cluster_table(ctx,
                                (const vgfx3d_cluster_table_t *)cmd->cluster_table,
                                cmd->lights_revision,
                                light_count);
        ctx->uploaded_cluster_revision = cmd->lights_revision;
    }

    bind_morph_payload(ctx,
                       cmd,
                       ctx->uHasSkinning,
                       ctx->uInstanceBoneStride,
                       ctx->uMorphShapeCount,
                       ctx->uVertexCount,
                       ctx->uMorphWeights,
                       ctx->uPrevMorphWeights,
                       ctx->uMorphDeltas,
                       ctx->uHasMorphNormalDeltas,
                       ctx->uMorphNormalDeltas);
}

/// @brief Resolve every cmd texture from the cache and bind to its sampler unit.
///
/// Slot map: 0=diffuse, 1=normal, 2=specular, 3=emissive, 4-7=shadow,
/// 8=splat-control, 9-12=splat layers, 13=env cubemap, 14=metallic-
/// rough, 15=AO. Slots 16/17 are reserved for morph TBOs (set by
/// `bind_morph_payload`). Each `has*` uniform tells the shader which
/// slots are populated.
/// @param ctx Borrowed OpenGL context owning texture caches and sampler objects.
/// @param cmd Borrowed draw command containing every material texture source and sampler state.
static void bind_material_textures(gl_context_t *ctx, const vgfx3d_draw_cmd_t *cmd) {
    GLuint diffuse_tex = gl_get_material_texture(
        ctx,
        cmd->texture_asset,
        cmd->texture,
        cmd->texture_asset_cache_key[RT_MATERIAL3D_TEXTURE_SLOT_BASE_COLOR],
        cmd->texture_asset_mip_start[RT_MATERIAL3D_TEXTURE_SLOT_BASE_COLOR],
        cmd->texture_asset_mip_count[RT_MATERIAL3D_TEXTURE_SLOT_BASE_COLOR]);
    GLuint normal_tex =
        gl_get_material_texture(ctx,
                                cmd->normal_map_asset,
                                cmd->normal_map,
                                cmd->texture_asset_cache_key[RT_MATERIAL3D_TEXTURE_SLOT_NORMAL],
                                cmd->texture_asset_mip_start[RT_MATERIAL3D_TEXTURE_SLOT_NORMAL],
                                cmd->texture_asset_mip_count[RT_MATERIAL3D_TEXTURE_SLOT_NORMAL]);
    GLuint specular_tex =
        gl_get_material_texture(ctx,
                                cmd->specular_map_asset,
                                cmd->specular_map,
                                cmd->texture_asset_cache_key[RT_MATERIAL3D_TEXTURE_SLOT_SPECULAR],
                                cmd->texture_asset_mip_start[RT_MATERIAL3D_TEXTURE_SLOT_SPECULAR],
                                cmd->texture_asset_mip_count[RT_MATERIAL3D_TEXTURE_SLOT_SPECULAR]);
    GLuint emissive_tex =
        gl_get_material_texture(ctx,
                                cmd->emissive_map_asset,
                                cmd->emissive_map,
                                cmd->texture_asset_cache_key[RT_MATERIAL3D_TEXTURE_SLOT_EMISSIVE],
                                cmd->texture_asset_mip_start[RT_MATERIAL3D_TEXTURE_SLOT_EMISSIVE],
                                cmd->texture_asset_mip_count[RT_MATERIAL3D_TEXTURE_SLOT_EMISSIVE]);
    GLuint metallic_roughness_tex = gl_get_material_texture(
        ctx,
        cmd->metallic_roughness_map_asset,
        cmd->metallic_roughness_map,
        cmd->texture_asset_cache_key[RT_MATERIAL3D_TEXTURE_SLOT_METALLIC_ROUGHNESS],
        cmd->texture_asset_mip_start[RT_MATERIAL3D_TEXTURE_SLOT_METALLIC_ROUGHNESS],
        cmd->texture_asset_mip_count[RT_MATERIAL3D_TEXTURE_SLOT_METALLIC_ROUGHNESS]);
    GLuint ao_tex =
        gl_get_material_texture(ctx,
                                cmd->ao_map_asset,
                                cmd->ao_map,
                                cmd->texture_asset_cache_key[RT_MATERIAL3D_TEXTURE_SLOT_AO],
                                cmd->texture_asset_mip_start[RT_MATERIAL3D_TEXTURE_SLOT_AO],
                                cmd->texture_asset_mip_count[RT_MATERIAL3D_TEXTURE_SLOT_AO]);
    GLuint env_tex =
        cmd->env_map ? gl_get_cached_cubemap(ctx, (const rt_cubemap3d *)cmd->env_map) : 0;
    GLuint splat_tex = cmd->splat_map ? gl_get_cached_texture(ctx, cmd->splat_map) : 0;
    GLuint splat_layer0 =
        cmd->splat_layers[0] ? gl_get_cached_texture(ctx, cmd->splat_layers[0]) : 0;
    GLuint splat_layer1 =
        cmd->splat_layers[1] ? gl_get_cached_texture(ctx, cmd->splat_layers[1]) : 0;
    GLuint splat_layer2 =
        cmd->splat_layers[2] ? gl_get_cached_texture(ctx, cmd->splat_layers[2]) : 0;
    GLuint splat_layer3 =
        cmd->splat_layers[3] ? gl_get_cached_texture(ctx, cmd->splat_layers[3]) : 0;
    int has_splat = vgfx3d_opengl_has_complete_splat(cmd->has_splat,
                                                     splat_tex != 0,
                                                     splat_layer0 != 0,
                                                     splat_layer1 != 0,
                                                     splat_layer2 != 0,
                                                     splat_layer3 != 0);

    gl.Uniform1i(ctx->uHasTexture, diffuse_tex ? 1 : 0);
    gl.Uniform1i(ctx->uHasNormalMap, normal_tex ? 1 : 0);
    gl.Uniform1i(ctx->uHasSpecularMap, specular_tex ? 1 : 0);
    gl.Uniform1i(ctx->uHasEmissiveMap, emissive_tex ? 1 : 0);
    gl.Uniform1i(ctx->uHasMetallicRoughnessMap, metallic_roughness_tex ? 1 : 0);
    gl.Uniform1i(ctx->uHasAOMap, ao_tex ? 1 : 0);
    gl.Uniform1i(ctx->uHasSplat, has_splat ? 1 : 0);

    bind_texture_unit_with_sampler(ctx,
                                   ctx->uDiffuseTex,
                                   0,
                                   GL_TEXTURE_2D,
                                   diffuse_tex,
                                   cmd,
                                   RT_MATERIAL3D_TEXTURE_SLOT_BASE_COLOR);
    bind_texture_unit_with_sampler(
        ctx, ctx->uNormalTex, 1, GL_TEXTURE_2D, normal_tex, cmd, RT_MATERIAL3D_TEXTURE_SLOT_NORMAL);
    bind_texture_unit_with_sampler(ctx,
                                   ctx->uSpecularTex,
                                   2,
                                   GL_TEXTURE_2D,
                                   specular_tex,
                                   cmd,
                                   RT_MATERIAL3D_TEXTURE_SLOT_SPECULAR);
    bind_texture_unit_with_sampler(ctx,
                                   ctx->uEmissiveTex,
                                   3,
                                   GL_TEXTURE_2D,
                                   emissive_tex,
                                   cmd,
                                   RT_MATERIAL3D_TEXTURE_SLOT_EMISSIVE);
    /* ADR 0246: one array texture carries every shadow slot, so a single bind
     * covers the CSM cascades and the general/point-light slots alike. Per-slot
     * completeness is enforced by uShadowCount plus gl_sanitize_shadow_slot on
     * the render side, so there is nothing to gate here. */
    bind_texture_unit(
        ctx->uShadowArray, GL_TU_SHADOW_ARRAY, GL_TEXTURE_2D_ARRAY, ctx->shadow_array_tex);
    /* Plan 10: soft-particle draws never carry splat maps, so the snapshot
     * borrows the splat-control unit (GL 3.3's 16-unit budget is fully
     * assigned). The shader-side fade is force-disabled whenever a draw has
     * splat data, so the aliasing can never misread. */
    if (cmd->soft_particle_fade > 0.0f && !has_splat && ctx->opaque_depth_valid &&
        ctx->opaque_depth_tex)
        bind_texture_unit(
            ctx->uOpaqueDepthTex, GL_TU_SPLAT_CONTROL, GL_TEXTURE_2D, ctx->opaque_depth_tex);
    else
        bind_texture_unit(ctx->uSplatTex, GL_TU_SPLAT_CONTROL, GL_TEXTURE_2D, splat_tex);
    bind_texture_unit(ctx->uSplatLayer0, GL_TU_SPLAT_LAYER0, GL_TEXTURE_2D, splat_layer0);
    bind_texture_unit(ctx->uSplatLayer1, GL_TU_SPLAT_LAYER0 + 1, GL_TEXTURE_2D, splat_layer1);
    bind_texture_unit(ctx->uSplatLayer2, GL_TU_SPLAT_LAYER0 + 2, GL_TEXTURE_2D, splat_layer2);
    bind_texture_unit(ctx->uSplatLayer3, GL_TU_SPLAT_LAYER0 + 3, GL_TEXTURE_2D, splat_layer3);
    /* ADR 0246: the LUT has its own unit now. It used to alias splat layer 3, so
     * a terrain draw evicted it and the shader silently fell back to an analytic
     * environment BRDF — terrain now gets the same exact table as every other
     * material, matching D3D11's dedicated t18. */
    bind_texture_unit(ctx->uBrdfLut, GL_TU_BRDF_LUT, GL_TEXTURE_2D, ctx->brdf_lut_tex);
    bind_texture_unit(ctx->uEnvMap, GL_TU_ENV_MAP, GL_TEXTURE_CUBE_MAP, env_tex);
    bind_texture_unit_with_sampler(ctx,
                                   ctx->uMetallicRoughnessTex,
                                   GL_TU_METALLIC_ROUGHNESS,
                                   GL_TEXTURE_2D,
                                   metallic_roughness_tex,
                                   cmd,
                                   RT_MATERIAL3D_TEXTURE_SLOT_METALLIC_ROUGHNESS);
    bind_texture_unit_with_sampler(
        ctx, ctx->uAOTex, GL_TU_AO, GL_TEXTURE_2D, ao_tex, cmd, RT_MATERIAL3D_TEXTURE_SLOT_AO);
    if (ctx->uSplatScales >= 0) {
        gl.Uniform4f(ctx->uSplatScales,
                     cmd->splat_layer_scales[0],
                     cmd->splat_layer_scales[1],
                     cmd->splat_layer_scales[2],
                     cmd->splat_layer_scales[3]);
    }
    gl.ActiveTexture(GL_TEXTURE0);
}

/// @brief Set up animation uniforms for the depth-only shadow program.
///
/// Slimmer than the main pass — shadow depth doesn't need normals or
/// previous-frame morph weights. Reuses `bind_morph_payload` with -1
/// for the unused locations so it skips those uniform writes.
/// @param ctx Borrowed OpenGL context containing the active shadow program.
/// @param cmd Borrowed draw command containing skeletal and morph animation payloads.
static void bind_shadow_anim(gl_context_t *ctx, const vgfx3d_draw_cmd_t *cmd) {
    bind_morph_payload(ctx,
                       cmd,
                       ctx->shadow_uHasSkinning,
                       -1, /* shadow program has no per-instance palette stride */
                       ctx->shadow_uMorphShapeCount,
                       ctx->shadow_uVertexCount,
                       ctx->shadow_uMorphWeights,
                       -1,
                       ctx->shadow_uMorphDeltas,
                       -1,
                       -1);
}

/// @brief Composite the offscreen scene to the swapchain backbuffer.
///
/// Convenience wrapper around `gl_apply_postfx_chain` that targets the display surface, falling
/// back to an offscreen final target when the GLX default framebuffer cannot write RGB.
/// @param ctx Borrowed OpenGL context containing the completed scene texture.
/// @param chain Borrowed post-processing chain, or NULL for a passthrough resolve.
/// @return 1 when the scene is composited to the selected final target, otherwise 0.
static int draw_scene_texture(gl_context_t *ctx, const vgfx3d_postfx_chain_t *chain) {
    int32_t source_width;
    int32_t source_height;
    GLuint final_framebuffer;
    GLenum final_draw_buffer;

    if (!ctx || !ctx->scene_color_tex || ctx->width <= 0 || ctx->height <= 0)
        return 0;
    source_width = ctx->scene_width > 0 ? ctx->scene_width : ctx->width;
    source_height = ctx->scene_height > 0 ? ctx->scene_height : ctx->height;
    final_framebuffer = 0;
    final_draw_buffer = ctx->default_draw_buffer;
    if (!ctx->default_framebuffer_writable) {
        if (ensure_present_target(ctx, ctx->width, ctx->height) != 0)
            return 0;
        final_framebuffer = ctx->present_fbo;
        final_draw_buffer = GL_COLOR_ATTACHMENT0;
    }
    if (vgfx3d_postfx_chain_is_usable(chain) && gl_apply_postfx_chain(ctx,
                                                                      ctx->scene_color_tex,
                                                                      source_width,
                                                                      source_height,
                                                                      chain,
                                                                      final_framebuffer,
                                                                      final_draw_buffer,
                                                                      ctx->width,
                                                                      ctx->height,
                                                                      0,
                                                                      NULL,
                                                                      NULL)) {
        return 1;
    }
    if (vgfx3d_postfx_chain_is_usable(chain) && !ctx->default_framebuffer_writable &&
        gl_apply_postfx_chain_in_scene(ctx, source_width, source_height, chain)) {
        return gl_draw_texture_to_target(ctx,
                                         ctx->scene_color_tex,
                                         final_framebuffer,
                                         final_draw_buffer,
                                         source_width,
                                         source_height,
                                         ctx->width,
                                         ctx->height,
                                         NULL);
    }
    if (!ctx->postfx_program)
        return 0;
    return gl_draw_texture_to_target(ctx,
                                     ctx->scene_color_tex,
                                     final_framebuffer,
                                     final_draw_buffer,
                                     source_width,
                                     source_height,
                                     ctx->width,
                                     ctx->height,
                                     NULL);
}

/// @brief Internal skybox draw — full-screen triangle with reconstructed view rays.
///
/// Reconstructs a world-space ray from the inverse projection and inverse
/// rotation-only view matrix so perspective and orthographic cameras share one
/// consistent skybox path.
/// @param ctx Borrowed OpenGL context with an active target and skybox program.
/// @param cubemap Borrowed runtime cubemap to resolve through the streaming cache.
static void gl_draw_skybox_impl(gl_context_t *ctx, const rt_cubemap3d *cubemap) {
    static const float kIdentity4x4[16] = {
        1.0f,
        0.0f,
        0.0f,
        0.0f,
        0.0f,
        1.0f,
        0.0f,
        0.0f,
        0.0f,
        0.0f,
        1.0f,
        0.0f,
        0.0f,
        0.0f,
        0.0f,
        1.0f,
    };
    if (!ctx || !cubemap || !ctx->skybox_program)
        return;

    GLuint tex = gl_get_cached_cubemap(ctx, cubemap);
    if (!tex)
        return;

    float view_rotation[16];
    float inverse_projection[16];
    float inverse_view_rotation[16];
    memcpy(view_rotation, ctx->view, sizeof(view_rotation));
    view_rotation[3] = 0.0f;
    view_rotation[7] = 0.0f;
    view_rotation[11] = 0.0f;
    view_rotation[12] = 0.0f;
    view_rotation[13] = 0.0f;
    view_rotation[14] = 0.0f;
    view_rotation[15] = 1.0f;
    if (mat4f_inverse_gl(ctx->projection, inverse_projection) != 0)
        memcpy(inverse_projection, kIdentity4x4, sizeof(inverse_projection));
    transpose4x4(view_rotation, inverse_view_rotation);

    if (ctx->active_target_kind == VGFX3D_OPENGL_TARGET_SCENE) {
        gl.BindFramebuffer(GL_FRAMEBUFFER, ctx->scene_fbo);
        gl.DrawBuffer(GL_COLOR_ATTACHMENT0);
    }

    gl.DepthMask(GL_FALSE);
    /* Reversed-Z: the skybox triangle sits at the cleared far depth. */
    gl.DepthFunc(GL_GEQUAL);
    gl.Disable(GL_CULL_FACE);
    gl.UseProgram(ctx->skybox_program);
    gl.BindVertexArray(ctx->skybox_vao);
    gl.UniformMatrix4fv(ctx->skybox_uInverseProjection, 1, GL_TRUE, inverse_projection);
    gl.UniformMatrix4fv(ctx->skybox_uInverseViewRotation, 1, GL_TRUE, inverse_view_rotation);
    gl.Uniform3f(
        ctx->skybox_uDirection, ctx->cam_forward[0], ctx->cam_forward[1], ctx->cam_forward[2]);
    gl.Uniform1i(ctx->skybox_uIsOrtho, ctx->cam_is_ortho ? 1 : 0);
    bind_texture_unit(ctx->skybox_uSkybox, GL_TU_ENV_MAP, GL_TEXTURE_CUBE_MAP, tex);
    gl.DrawArrays(GL_TRIANGLES, 0, 3);

    gl.ActiveTexture(GL_TEXTURE0);
    gl.DepthFunc(GL_GREATER);
    gl.DepthMask(GL_TRUE);
    gl.Enable(GL_CULL_FACE);
    gl.UseProgram(ctx->program);
    gl.BindVertexArray(ctx->vao);
    if (ctx->active_target_kind == VGFX3D_OPENGL_TARGET_SCENE)
        bind_main_framebuffer(ctx);
}

/// @brief Resolve every uniform location in the main program.
///
/// Called once at context-create time. Uses the `U(name)` macro to
/// keep the boilerplate compact. Per-light array uniforms are queried
/// individually with `snprintf`-built names like `uLightPos[3]`.
/// Locations of -1 (uniform doesn't exist or was optimized out) are
/// fine — the per-frame `glUniform*` calls handle them gracefully.
/// @param ctx Borrowed OpenGL context receiving locations from its current main program.
static void query_main_uniforms(gl_context_t *ctx) {
#define U(name) ctx->name = gl.GetUniformLocation(ctx->program, #name)
    U(uModelMatrix);
    U(uPrevModelMatrix);
    U(uViewProjection);
    U(uPrevViewProjection);
    U(uNormalMatrix);
    U(uCameraPos);
    U(uCameraForward);
    U(uAmbientColor);
    U(uDiffuseColor);
    U(uSpecularColor);
    U(uEmissiveColor);
    U(uAlpha);
    U(uPbrScalars0);
    U(uPbrScalars1);
    U(uUnlit);
    U(uShadingModel);
    U(uLightCount);
    U(uHasTexture);
    U(uHasNormalMap);
    U(uHasSpecularMap);
    U(uHasEmissiveMap);
    U(uHasEnvMap);
    U(uReflectivity);
    U(uEnvMaxLod);
    U(uWorkflow);
    U(uAlphaMode);
    U(uCameraIsOrtho);
    U(uHasMetallicRoughnessMap);
    U(uHasAOMap);
    U(uCustomParams);
    U(uTextureUvSets0);
    U(uTextureUvSets1);
    ctx->uTextureUvTransform0 = gl.GetUniformLocation(ctx->program, "uTextureUvTransform0[0]");
    ctx->uTextureUvTransform1 = gl.GetUniformLocation(ctx->program, "uTextureUvTransform1[0]");
    U(uHasSplat);
    U(uFogEnabled);
    U(uFogNear);
    U(uHeightFog);
    U(uHeightFogSun);
    U(uHeightFogSunDir);
    U(uFogFar);
    U(uFogColor);
    U(uIblEnabled);
    U(uIblIntensity);
    U(uEnvLodBase);
    ctx->uShIrradiance = gl.GetUniformLocation(ctx->program, "uShIrradiance[0]");
    U(uShadowCount);
    U(uShadowBias);
    U(uShadowSlopeBias);
    U(uShadowStrength);
    U(uShadowSampleCount);
    U(uClusterGlobalCount);
    U(uClusterParams);
    U(uOpaqueDepthTex);
    U(uUseInstancing);
    U(uParticleMode);
    U(uHasPrevModelMatrix);
    U(uHasPrevInstanceMatrices);
    U(uHasSkinning);
    U(uHasPrevSkinning);
    U(uInstanceBoneStride);
    U(uMorphShapeCount);
    U(uVertexCount);
    U(uHasPrevMorphWeights);
    ctx->uMorphWeights = gl.GetUniformLocation(ctx->program, "uMorphWeights[0]");
    ctx->uPrevMorphWeights = gl.GetUniformLocation(ctx->program, "uPrevMorphWeights[0]");
    U(uMorphDeltas);
    U(uMorphNormalDeltas);
    U(uHasMorphNormalDeltas);
    U(uDiffuseTex);
    U(uNormalTex);
    U(uSpecularTex);
    U(uEmissiveTex);
    U(uEnvMap);
    U(uBrdfLut);
    U(uMetallicRoughnessTex);
    U(uAOTex);
    U(uSplatTex);
    U(uSplatLayer0);
    U(uSplatLayer1);
    U(uSplatLayer2);
    U(uSplatLayer3);
    U(uSplatScales);
#undef U
    /* The GL 3.3 baseline exposes only 16 fragment samplers. Soft-particle
     * depth and terrain splatting are mutually exclusive, so the shader uses
     * uSplatTex for both and the CPU-facing locations intentionally alias. */
    ctx->uOpaqueDepthTex = ctx->uSplatTex;
    for (int i = 0; i < VGFX3D_MAX_LIGHTS; i++) {
        char name[64];
        snprintf(name, sizeof(name), "uLightType[%d]", i);
        ctx->uLightType[i] = gl.GetUniformLocation(ctx->program, name);
        snprintf(name, sizeof(name), "uLightShadowIndex[%d]", i);
        ctx->uLightShadowIndex[i] = gl.GetUniformLocation(ctx->program, name);
        snprintf(name, sizeof(name), "uLightShadowCascadeCount[%d]", i);
        ctx->uLightShadowCascadeCount[i] = gl.GetUniformLocation(ctx->program, name);
        snprintf(name, sizeof(name), "uLightShadowProjectionType[%d]", i);
        ctx->uLightShadowProjectionType[i] = gl.GetUniformLocation(ctx->program, name);
        snprintf(name, sizeof(name), "uLightShadowCascadeSplits[%d]", i);
        ctx->uLightShadowCascadeSplits[i] = gl.GetUniformLocation(ctx->program, name);
        snprintf(name, sizeof(name), "uLightDir[%d]", i);
        ctx->uLightDir[i] = gl.GetUniformLocation(ctx->program, name);
        snprintf(name, sizeof(name), "uLightPos[%d]", i);
        ctx->uLightPos[i] = gl.GetUniformLocation(ctx->program, name);
        snprintf(name, sizeof(name), "uLightColor[%d]", i);
        ctx->uLightColor[i] = gl.GetUniformLocation(ctx->program, name);
        snprintf(name, sizeof(name), "uLightIntensity[%d]", i);
        ctx->uLightIntensity[i] = gl.GetUniformLocation(ctx->program, name);
        snprintf(name, sizeof(name), "uLightAtten[%d]", i);
        ctx->uLightAtten[i] = gl.GetUniformLocation(ctx->program, name);
        snprintf(name, sizeof(name), "uLightInnerCos[%d]", i);
        ctx->uLightInnerCos[i] = gl.GetUniformLocation(ctx->program, name);
        snprintf(name, sizeof(name), "uLightOuterCos[%d]", i);
        ctx->uLightOuterCos[i] = gl.GetUniformLocation(ctx->program, name);
    }
    for (int i = 0; i < VGFX3D_MAX_SHADOW_LIGHTS; i++) {
        char name[64];
        snprintf(name, sizeof(name), "uShadowVP[%d]", i);
        ctx->uShadowVP[i] = gl.GetUniformLocation(ctx->program, name);
    }
    ctx->uShadowArray = gl.GetUniformLocation(ctx->program, "uShadowArray");
}

/// @brief Measure the contiguous context-memory span occupied by main uniform locations.
/// @param ctx Borrowed OpenGL context whose location fields define the span.
/// @return Number of bytes from `uModelMatrix` through the final light-location array.
static size_t gl_main_uniform_location_span(gl_context_t *ctx) {
    return (size_t)((unsigned char *)(&ctx->uLightOuterCos[VGFX3D_MAX_LIGHTS]) -
                    (unsigned char *)&ctx->uModelMatrix);
}

/// @brief Copy the current main-program uniform-location span into caller storage.
/// @param ctx Borrowed OpenGL context providing the location block and its measured size.
/// @param[out] storage Caller-owned buffer large enough for `main_uniform_location_bytes`.
static void gl_save_main_uniform_locations(gl_context_t *ctx, GLint *storage) {
    memcpy(storage, &ctx->uModelMatrix, ctx->main_uniform_location_bytes);
}

/// @brief Restore a previously saved uniform-location block into the context.
/// @param ctx Borrowed OpenGL context receiving the location block.
/// @param storage Borrowed buffer previously populated by gl_save_main_uniform_locations().
static void gl_load_main_uniform_locations(gl_context_t *ctx, const GLint *storage) {
    memcpy(&ctx->uModelMatrix, storage, ctx->main_uniform_location_bytes);
}

/// @brief Switch eligible unlit draws to the compact specialized shader program.
/// @param ctx Borrowed OpenGL context whose active program and locations may be changed.
/// @param cmd Borrowed draw command used to determine specialization eligibility.
/// @param[out] saved_locations Caller-owned storage receiving full-program uniform locations.
/// @param[out] saved_program Caller-owned destination for the full program object name.
/// @return 1 when the specialized program was selected, otherwise 0.
static int gl_begin_material_program(gl_context_t *ctx,
                                     const vgfx3d_draw_cmd_t *cmd,
                                     GLint *saved_locations,
                                     GLuint *saved_program) {
    if (!ctx || !cmd || !cmd->unlit || cmd->env_map || !ctx->unlit_program)
        return 0;
    *saved_program = ctx->program;
    gl_save_main_uniform_locations(ctx, saved_locations);
    gl_load_main_uniform_locations(ctx, ctx->unlit_uniform_locations);
    ctx->program = ctx->unlit_program;
    return 1;
}

/// @brief Restore the full material program after an optional specialized draw.
/// @param ctx Borrowed OpenGL context whose program and locations are restored.
/// @param specialized Nonzero when gl_begin_material_program() performed a switch.
/// @param saved_locations Borrowed full-program location snapshot.
/// @param saved_program Full shader program object name to restore.
static void gl_end_material_program(gl_context_t *ctx,
                                    int specialized,
                                    const GLint *saved_locations,
                                    GLuint saved_program) {
    if (!specialized)
        return;
    ctx->program = saved_program;
    gl_load_main_uniform_locations(ctx, saved_locations);
    /* The specialized upload intentionally skips full-program light state. */
    ctx->uploaded_lights_revision = 0;
    ctx->uploaded_cluster_revision = 0;
}

/// @brief Query and retain the specialized unlit program's uniform-location layout.
/// @param ctx Borrowed OpenGL context containing linked full and unlit programs.
/// @return 1 when the location span fits internal snapshots and is captured, otherwise 0.
static int gl_query_unlit_uniforms(gl_context_t *ctx) {
    GLint full_locations[GL_MAIN_UNIFORM_SNAPSHOT_CAPACITY];
    GLuint full_program;
    if (!ctx || !ctx->unlit_program)
        return 0;
    ctx->main_uniform_location_bytes = gl_main_uniform_location_span(ctx);
    if (ctx->main_uniform_location_bytes > sizeof(ctx->unlit_uniform_locations) ||
        ctx->main_uniform_location_bytes > sizeof(full_locations))
        return 0;
    full_program = ctx->program;
    gl_save_main_uniform_locations(ctx, full_locations);
    ctx->program = ctx->unlit_program;
    query_main_uniforms(ctx);
    gl_save_main_uniform_locations(ctx, ctx->unlit_uniform_locations);
    ctx->program = full_program;
    gl_load_main_uniform_locations(ctx, full_locations);
    return 1;
}

/// @brief Resolve uniform locations for the depth-only shadow program.
/// @param ctx Borrowed OpenGL context receiving shadow-program uniform locations.
static void query_shadow_uniforms(gl_context_t *ctx) {
    ctx->shadow_uModelMatrix = gl.GetUniformLocation(ctx->shadow_program, "uModelMatrix");
    ctx->shadow_uViewProjection = gl.GetUniformLocation(ctx->shadow_program, "uViewProjection");
    ctx->shadow_uHasSkinning = gl.GetUniformLocation(ctx->shadow_program, "uHasSkinning");
    ctx->shadow_uMorphShapeCount = gl.GetUniformLocation(ctx->shadow_program, "uMorphShapeCount");
    ctx->shadow_uVertexCount = gl.GetUniformLocation(ctx->shadow_program, "uVertexCount");
    ctx->shadow_uMorphWeights = gl.GetUniformLocation(ctx->shadow_program, "uMorphWeights[0]");
    ctx->shadow_uMorphDeltas = gl.GetUniformLocation(ctx->shadow_program, "uMorphDeltas");
    ctx->shadow_uDiffuseTex = gl.GetUniformLocation(ctx->shadow_program, "uShadowDiffuseTex");
    ctx->shadow_uHasTexture = gl.GetUniformLocation(ctx->shadow_program, "uShadowHasTexture");
    ctx->shadow_uAlphaMode = gl.GetUniformLocation(ctx->shadow_program, "uShadowAlphaMode");
    ctx->shadow_uAlphaCutoff = gl.GetUniformLocation(ctx->shadow_program, "uShadowAlphaCutoff");
    ctx->shadow_uAlpha = gl.GetUniformLocation(ctx->shadow_program, "uShadowAlpha");
    ctx->shadow_uDiffuseColor = gl.GetUniformLocation(ctx->shadow_program, "uShadowDiffuseColor");
    ctx->shadow_uTextureUvSets0 =
        gl.GetUniformLocation(ctx->shadow_program, "uShadowTextureUvSets0");
    ctx->shadow_uTextureUvSets1 =
        gl.GetUniformLocation(ctx->shadow_program, "uShadowTextureUvSets1");
    ctx->shadow_uTextureUvTransform0 =
        gl.GetUniformLocation(ctx->shadow_program, "uShadowTextureUvTransform0[0]");
    ctx->shadow_uTextureUvTransform1 =
        gl.GetUniformLocation(ctx->shadow_program, "uShadowTextureUvTransform1[0]");
}

/// @brief Resolve uniform locations for the cubemap skybox program.
/// @param ctx Borrowed OpenGL context receiving skybox-program uniform locations.
static void query_skybox_uniforms(gl_context_t *ctx) {
    ctx->skybox_uInverseProjection =
        gl.GetUniformLocation(ctx->skybox_program, "uInverseProjection");
    ctx->skybox_uInverseViewRotation =
        gl.GetUniformLocation(ctx->skybox_program, "uInverseViewRotation");
    ctx->skybox_uDirection = gl.GetUniformLocation(ctx->skybox_program, "uDirection");
    ctx->skybox_uIsOrtho = gl.GetUniformLocation(ctx->skybox_program, "uIsOrtho");
    ctx->skybox_uSkybox = gl.GetUniformLocation(ctx->skybox_program, "uSkybox");
}

/// @brief Resolve every uniform in the post-FX shader (bloom, tonemap, FXAA, etc.).
/// @param ctx Borrowed OpenGL context receiving post-processing uniform locations.
static void query_postfx_uniforms(gl_context_t *ctx) {
    ctx->postfx_uSceneTex = gl.GetUniformLocation(ctx->postfx_program, "uSceneTex");
    ctx->postfx_uSceneDepthTex = gl.GetUniformLocation(ctx->postfx_program, "uSceneDepthTex");
    ctx->postfx_uSceneMotionTex = gl.GetUniformLocation(ctx->postfx_program, "uSceneMotionTex");
    ctx->postfx_uInvResolution = gl.GetUniformLocation(ctx->postfx_program, "uInvResolution");
    ctx->postfx_uBloomEnabled = gl.GetUniformLocation(ctx->postfx_program, "uBloomEnabled");
    ctx->postfx_uBloomThreshold = gl.GetUniformLocation(ctx->postfx_program, "uBloomThreshold");
    ctx->postfx_uBloomIntensity = gl.GetUniformLocation(ctx->postfx_program, "uBloomIntensity");
    ctx->postfx_uBloomPasses = gl.GetUniformLocation(ctx->postfx_program, "uBloomPasses");
    ctx->postfx_uTonemapMode = gl.GetUniformLocation(ctx->postfx_program, "uTonemapMode");
    ctx->postfx_uTonemapExposure = gl.GetUniformLocation(ctx->postfx_program, "uTonemapExposure");
    ctx->postfx_uFxaaEnabled = gl.GetUniformLocation(ctx->postfx_program, "uFxaaEnabled");
    ctx->postfx_uColorGradeEnabled =
        gl.GetUniformLocation(ctx->postfx_program, "uColorGradeEnabled");
    ctx->postfx_uCgBrightness = gl.GetUniformLocation(ctx->postfx_program, "uCgBrightness");
    ctx->postfx_uCgContrast = gl.GetUniformLocation(ctx->postfx_program, "uCgContrast");
    ctx->postfx_uCgSaturation = gl.GetUniformLocation(ctx->postfx_program, "uCgSaturation");
    ctx->postfx_uVignetteEnabled = gl.GetUniformLocation(ctx->postfx_program, "uVignetteEnabled");
    ctx->postfx_uVignetteRadius = gl.GetUniformLocation(ctx->postfx_program, "uVignetteRadius");
    ctx->postfx_uVignetteSoftness = gl.GetUniformLocation(ctx->postfx_program, "uVignetteSoftness");
    ctx->postfx_uSsaoEnabled = gl.GetUniformLocation(ctx->postfx_program, "uSsaoEnabled");
    ctx->postfx_uSsaoRadius = gl.GetUniformLocation(ctx->postfx_program, "uSsaoRadius");
    ctx->postfx_uSsaoIntensity = gl.GetUniformLocation(ctx->postfx_program, "uSsaoIntensity");
    ctx->postfx_uSsaoSamples = gl.GetUniformLocation(ctx->postfx_program, "uSsaoSamples");
    ctx->postfx_uDofEnabled = gl.GetUniformLocation(ctx->postfx_program, "uDofEnabled");
    ctx->postfx_uDofFocusDistance = gl.GetUniformLocation(ctx->postfx_program, "uDofFocusDistance");
    ctx->postfx_uDofAperture = gl.GetUniformLocation(ctx->postfx_program, "uDofAperture");
    ctx->postfx_uDofMaxBlur = gl.GetUniformLocation(ctx->postfx_program, "uDofMaxBlur");
    ctx->postfx_uMotionBlurEnabled =
        gl.GetUniformLocation(ctx->postfx_program, "uMotionBlurEnabled");
    ctx->postfx_uMotionBlurIntensity =
        gl.GetUniformLocation(ctx->postfx_program, "uMotionBlurIntensity");
    ctx->postfx_uMotionBlurSamples =
        gl.GetUniformLocation(ctx->postfx_program, "uMotionBlurSamples");
    ctx->postfx_uCameraPos = gl.GetUniformLocation(ctx->postfx_program, "uCameraPos");
    ctx->postfx_uInvViewProjection =
        gl.GetUniformLocation(ctx->postfx_program, "uInvViewProjection");
    ctx->postfx_uPrevViewProjection =
        gl.GetUniformLocation(ctx->postfx_program, "uPrevViewProjection");
    ctx->postfx_uSceneHdr = gl.GetUniformLocation(ctx->postfx_program, "uSceneHdr");
    ctx->postfx_uTonemapExplicit = gl.GetUniformLocation(ctx->postfx_program, "uTonemapExplicit");
    ctx->postfx_uBloomTex = gl.GetUniformLocation(ctx->postfx_program, "uBloomTex");
    ctx->postfx_uBloomTexEnabled = gl.GetUniformLocation(ctx->postfx_program, "uBloomTexEnabled");
}

/// @brief Cache uniform locations for the Plan 05 bloom-chain and TAA pass programs.
/// @param ctx Borrowed OpenGL context receiving bloom, TAA, and SSR uniform locations.
static void query_bloom_taa_uniforms(gl_context_t *ctx) {
    ctx->bloom_down_uSrcTex = gl.GetUniformLocation(ctx->bloom_down_program, "uSrcTex");
    ctx->bloom_down_uSrcInvSize = gl.GetUniformLocation(ctx->bloom_down_program, "uSrcInvSize");
    ctx->bloom_down_uThreshold = gl.GetUniformLocation(ctx->bloom_down_program, "uThreshold");
    ctx->bloom_down_uFirstPass = gl.GetUniformLocation(ctx->bloom_down_program, "uFirstPass");
    ctx->bloom_up_uSrcTex = gl.GetUniformLocation(ctx->bloom_up_program, "uSrcTex");
    ctx->bloom_up_uSrcInvSize = gl.GetUniformLocation(ctx->bloom_up_program, "uSrcInvSize");
    ctx->taa_uCurrTex = gl.GetUniformLocation(ctx->taa_program, "uCurrTex");
    ctx->taa_uHistTex = gl.GetUniformLocation(ctx->taa_program, "uHistTex");
    ctx->taa_uMotionTex = gl.GetUniformLocation(ctx->taa_program, "uMotionTex");
    ctx->taa_uDepthTex = gl.GetUniformLocation(ctx->taa_program, "uDepthTex");
    ctx->taa_uInvResolution = gl.GetUniformLocation(ctx->taa_program, "uInvResolution");
    ctx->taa_uJitterDelta = gl.GetUniformLocation(ctx->taa_program, "uJitterDelta");
    ctx->taa_uBlend = gl.GetUniformLocation(ctx->taa_program, "uBlend");
    ctx->taa_uHistoryValid = gl.GetUniformLocation(ctx->taa_program, "uHistoryValid");
    ctx->taa_uInvViewProjection = gl.GetUniformLocation(ctx->taa_program, "uInvViewProjection");
    ctx->taa_uPrevViewProjection = gl.GetUniformLocation(ctx->taa_program, "uPrevViewProjection");
    ctx->ssr_uSceneTex = gl.GetUniformLocation(ctx->ssr_program, "uSceneTex");
    ctx->ssr_uDepthTex = gl.GetUniformLocation(ctx->ssr_program, "uDepthTex");
    ctx->ssr_uMotionTex = gl.GetUniformLocation(ctx->ssr_program, "uMotionTex");
    ctx->ssr_uInvResolution = gl.GetUniformLocation(ctx->ssr_program, "uInvResolution");
    ctx->ssr_uParams0 = gl.GetUniformLocation(ctx->ssr_program, "uParams0");
    ctx->ssr_uCamPos = gl.GetUniformLocation(ctx->ssr_program, "uCamPos");
    ctx->ssr_uInvViewProjection = gl.GetUniformLocation(ctx->ssr_program, "uInvViewProjection");
    ctx->ssr_uViewProjection = gl.GetUniformLocation(ctx->ssr_program, "uViewProjection");
}
