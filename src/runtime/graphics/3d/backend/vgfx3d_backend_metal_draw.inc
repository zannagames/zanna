//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/backend/vgfx3d_backend_metal_draw.inc
// Purpose: Metal mesh/instance/shadow submission, presentation, readback, and post-FX encoding.
// Key invariants:
//   - Every encoded draw binds a target-compatible pipeline and complete resource snapshot.
//   - Compact particle records receive an independent transient-ring slice for each deferred batch.
// Ownership/Lifetime:
//   - Encoders and transient buffers belong to VGFXMetalContext for the active in-flight frame.
//   - Canvas command memory is borrowed only while the synchronous backend hook is encoding it.
// Links: vgfx3d_backend_metal.m, vgfx3d_backend_metal_context.inc
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements Metal draw submission, shadow passes, presentation, readback, and post-FX.
/// @details This implementation fragment translates sanitized renderer commands into Metal
///          encoders, manages per-frame target transitions, and exposes the draw-related hooks
///          installed in the Metal backend vtable. It is included by `vgfx3d_backend_metal.m`.

/// @brief Encode and submit a single 3D draw command (the backend's main draw entry point).
/// @details Selects/creates the pipeline state for the material, binds geometry, textures,
/// lighting,
///          skinning, and morph data, applies the camera-relative transform, and issues the draw
///          (instanced when an instance buffer is provided).
/// @param ctx_ptr Borrowed `VGFXMetalContext` that owns the active command encoder.
/// @param win Window handle supplied by the backend interface; unused by the Metal draw path.
/// @param cmd Borrowed mesh, material, animation, and per-draw state to encode.
/// @param lights Borrowed array of scene-light snapshots; may be NULL when @p light_count is zero.
/// @param light_count Number of entries available through @p lights.
/// @param ambient Borrowed three-component ambient-light color; NULL is sanitized to black.
/// @param wireframe Nonzero to render triangle edges instead of filled triangles.
/// @param backface_cull Nonzero to cull back-facing triangles.
static void metal_submit_draw(void *ctx_ptr,
                              vgfx_window_t win,
                              const vgfx3d_draw_cmd_t *cmd,
                              const vgfx3d_light_params_t *lights,
                              int32_t light_count,
                              const float *ambient,
                              int8_t wireframe,
                              int8_t backface_cull) {
    @autoreleasepool {
        (void)win;
        VGFXMetalContext *ctx = (__bridge VGFXMetalContext *)ctx_ptr;
        id<MTLBuffer> vb;
        id<MTLBuffer> ib;
        id<MTLTexture> env_texture = nil;
        NSUInteger vb_offset = 0;
        NSUInteger ib_offset = 0;
        uint32_t index_count;
        vgfx3d_metal_blend_mode_t blend_mode;
        vgfx3d_draw_cmd_t safe_cmd;
        vgfx3d_light_params_t safe_lights[VGFX3D_MAX_LIGHTS];
        float safe_ambient[3];
        if (!ctx)
            return;
        if (!ctx.encoder || !ctx.inFrame || !cmd || !cmd->vertices || !cmd->indices ||
            cmd->vertex_count == 0 || cmd->index_count == 0) {
            ctx->_stats.dropped_draws++;
            return;
        }
        vgfx3d_sanitize_draw_command(cmd, &safe_cmd);
        cmd = &safe_cmd;
        light_count =
            vgfx3d_sanitize_light_array(lights, light_count, safe_lights, VGFX3D_MAX_LIGHTS);
        for (int32_t light_index = 0; light_index < light_count; light_index++)
            vgfx3d_sanitize_light_shadow_span(&safe_lights[light_index], ctx->_shadowCount);
        lights = light_count > 0 ? safe_lights : NULL;
        vgfx3d_sanitize_ambient_rgb(ambient, safe_ambient);
        ambient = safe_ambient;
        index_count = vgfx3d_draw_cmd_validated_index_count(cmd);
        if (index_count == 0) {
            ctx->_stats.dropped_draws++;
            return;
        }

        blend_mode = vgfx3d_metal_choose_blend_mode(cmd);
        [ctx.encoder setRenderPipelineState:metal_select_pipeline_state(ctx, cmd, NO)];
        [ctx.encoder setCullMode:backface_cull ? MTLCullModeBack : MTLCullModeNone];

        /* MTL-08: Wireframe mode via fill mode toggle */
        [ctx.encoder
            setTriangleFillMode:wireframe ? MTLTriangleFillModeLines : MTLTriangleFillModeFill];

        /* Switch depth state: no Z-write for transparent draws */
        if (cmd->disable_depth_test)
            [ctx.encoder setDepthStencilState:ctx.depthStateDisabled];
        else if (blend_mode != VGFX3D_METAL_BLEND_OPAQUE)
            [ctx.encoder setDepthStencilState:ctx.depthStateNoWrite];
        else
            [ctx.encoder setDepthStencilState:ctx.depthState];
        /* Scene passes are reversed-Z: flip both bias terms so positive material
         * bias still pushes fragments away from the camera. The shadow pass below
         * keeps the standard convention. */
        if (fabsf(cmd->depth_bias) > 1e-8f || fabsf(cmd->slope_scaled_depth_bias) > 1e-8f)
            [ctx.encoder
                setDepthBias:vgfx3d_depth_bias_constant_units_reversed_z(cmd->depth_bias)
                  slopeScale:vgfx3d_depth_bias_slope_reversed_z(cmd->slope_scaled_depth_bias)
                       clamp:0.0f];
        else
            [ctx.encoder setDepthBias:0.0f slopeScale:0.0f clamp:0.0f];

        if (!metal_get_geometry_buffers(ctx, cmd, &vb, &ib, &vb_offset, &ib_offset)) {
            ctx->_stats.dropped_draws++;
            return;
        }
        [ctx.encoder setVertexBuffer:vb offset:vb_offset atIndex:0];

        /* Retain buffers until frame commit — prevents ARC from releasing
         * them before the GPU executes the draw commands. */
        if (ctx.frameBuffers) {
            [ctx.frameBuffers addObject:vb];
            [ctx.frameBuffers addObject:ib];
        }

        /* Per-object uniforms (includes MTL-09/10 skinning+morph flags) */
        mtl_per_object_t obj;
        float safe_model[16];
        float safe_prev_model[16];
        memset(&obj, 0, sizeof(obj));
        vgfx3d_copy_mat4_finite_or_identity(safe_model, cmd->model_matrix);
        vgfx3d_copy_mat4_finite_or(safe_prev_model,
                                   cmd->has_prev_model_matrix ? cmd->prev_model_matrix
                                                              : cmd->model_matrix,
                                   safe_model);
        transpose4x4(safe_model, obj.m);
        transpose4x4(safe_prev_model, obj.prev_m);
        float vp_t[16];
        float normal_m[16];
        int prev_bone_upload_ok = 0;
        metal_morph_bind_status_t morph_status;
        transpose4x4(ctx->_vp, vp_t);
        memcpy(obj.vp, vp_t, sizeof(float) * 16);
        vgfx3d_compute_normal_matrix4(safe_model, normal_m);
        transpose4x4(normal_m, obj.nm);
        int has_skinning = (cmd->bone_palette && cmd->bone_count > 0) ? 1 : 0;
        int has_prev_skinning = (has_skinning && cmd->prev_bone_palette) ? 1 : 0;
        has_skinning = metal_bind_bone_palettes(
            ctx, cmd, has_skinning, has_prev_skinning, &prev_bone_upload_ok);
        has_prev_skinning = (has_skinning && has_prev_skinning && prev_bone_upload_ok) ? 1 : 0;
        morph_status = metal_bind_morph_payload(ctx, cmd);
        obj.flags0[0] = has_skinning;
        obj.flags0[1] = has_prev_skinning;
        obj.flags0[2] = morph_status.shape_count;
        obj.flags0[3] = morph_status.shape_count > 0 ? (int32_t)cmd->vertex_count : 0;
        obj.flags1[0] = cmd->has_prev_model_matrix ? 1 : 0;
        obj.flags1[1] = morph_status.has_prev_weights;
        obj.flags1[2] = 0;
        obj.flags1[3] = morph_status.has_normal_deltas;
        obj.flags2[0] = cmd->instance_bone_stride;
        obj.flags2[1] = has_skinning ? metal_bind_extra_influences(ctx, cmd) : 0;
        [ctx.encoder setVertexBytes:&obj length:sizeof(obj) atIndex:1];

        /* Image-based lighting resolution for this draw (also used at env bind). */
        float ibl_env_lod_base = 0.0f;
        float ibl_env_max_lod = 0.0f;
        if (cmd->env_map && (cmd->reflectivity > 0.0001f || (cmd->ibl_env && ctx->_iblEnabled))) {
            env_texture = metal_get_cached_cubemap(ctx, (const rt_cubemap3d *)cmd->env_map);
        }
        int ibl_draw = metal_cmd_ibl_draw(ctx, cmd, &ibl_env_lod_base, &ibl_env_max_lod);
        /* Scene + light constants persist in the encoder; re-encode only when
         * the queued light-snapshot revision changes (0 = always encode). */
        int lights_dirty =
            (cmd->lights_revision == 0 || cmd->lights_revision != ctx->_encoderLightsRevision ||
             ambient[0] != ctx->_encoderAmbient[0] || ambient[1] != ctx->_encoderAmbient[1] ||
             ambient[2] != ctx->_encoderAmbient[2]);

        /* Per-scene (includes MTL-07 fog) */
        mtl_per_scene_t scene;
        id<MTLBuffer> clusterBuf = nil;
        if (lights_dirty) {
            memset(&scene, 0, sizeof(scene));
            memcpy(scene.cp, ctx->_camPos, sizeof(float) * 3);
            scene.cp[3] = ctx->_camIsOrtho ? 1.0f : 0.0f;
            scene.ac[0] = ambient[0];
            scene.ac[1] = ambient[1];
            scene.ac[2] = ambient[2];
            scene.fc[0] = ctx->_fogColor[0];
            scene.fc[1] = ctx->_fogColor[1];
            scene.fc[2] = ctx->_fogColor[2];
            scene.fc[3] = ctx->_fogEnabled ? 1.0f : 0.0f;
            scene.fog_params[0] = ctx->_fogNear;
            scene.fog_params[1] = ctx->_fogFar;
            scene.fog_params[2] = ctx->_shadowBias;
            memcpy(scene.height_fog, ctx->_heightFog, sizeof(scene.height_fog));
            memcpy(scene.height_fog_sun, ctx->_heightFogSun, sizeof(scene.height_fog_sun));
            memcpy(
                scene.height_fog_sun_dir, ctx->_heightFogSunDir, sizeof(scene.height_fog_sun_dir));
            scene.counts[0] = light_count < 0 ? 0
                                              : (light_count > VGFX3D_MAX_LIGHTS ? VGFX3D_MAX_LIGHTS
                                                                                 : light_count);
            /* MTL-12: Shadow mapping */
            scene.counts[1] = ctx->_shadowCount;
            /* Plan 07: clustered forward+. counts[2] >= 0 selects the clustered loop
             * (value = global dir/ambient prefix length); -1 keeps the flat loop. */
            clusterBuf =
                metal_cluster_buffer_for_table(ctx,
                                               (const vgfx3d_cluster_table_t *)cmd->cluster_table,
                                               cmd->lights_revision,
                                               light_count);
            scene.counts[2] =
                clusterBuf
                    ? ((const vgfx3d_cluster_table_t *)cmd->cluster_table)->global_light_count
                    : -1;
            scene.cluster_params[0] =
                (float)(ctx.rttActive ? ctx.rttWidth
                                      : (ctx.currentTargetKind == VGFX3D_METAL_TARGET_SCENE
                                             ? (int32_t)ctx.offscreenColor.width
                                             : ctx.width));
            scene.cluster_params[1] =
                (float)(ctx.rttActive ? ctx.rttHeight
                                      : (ctx.currentTargetKind == VGFX3D_METAL_TARGET_SCENE
                                             ? (int32_t)ctx.offscreenColor.height
                                             : ctx.height));
            /* Plan 10: spare lanes carry the clip planes for depth linearization. */
            scene.cluster_params[2] = ctx->_camNear;
            scene.cluster_params[3] = ctx->_camFar;
            memcpy(scene.camera_forward, ctx->_camForward, sizeof(float) * 3);
            scene.ibl_params[0] = ctx->_iblIntensity;
            scene.ibl_params[1] = ctx->_shadowSlopeBias;
            scene.ibl_params[2] = ctx->_shadowStrength;
            scene.ibl_params[3] =
                (float)(ctx->_shadowQuality <= 0 ? 4 : (ctx->_shadowQuality == 1 ? 8 : 16));
            for (int sh_k = 0; sh_k < 9; sh_k++) {
                scene.sh[sh_k][0] = ctx->_iblSh[sh_k * 3 + 0];
                scene.sh[sh_k][1] = ctx->_iblSh[sh_k * 3 + 1];
                scene.sh[sh_k][2] = ctx->_iblSh[sh_k * 3 + 2];
            }
            transpose4x4(ctx.frameHistory.draw_prev_vp, scene.prev_vp);
            for (int32_t slot = 0; slot < ctx->_shadowCount && slot < VGFX3D_MAX_SHADOW_LIGHTS;
                 slot++)
                transpose4x4(ctx->_shadowLightVP[slot], scene.shadow_vp[slot]);
            [ctx.encoder setVertexBytes:&scene length:sizeof(scene) atIndex:2];
            [ctx.encoder setFragmentBytes:&scene length:sizeof(scene) atIndex:0];
        }

        /* Per-material (includes MTL-04/05/06 map flags) */
        mtl_per_material_t mat;
        int has_complete_splat = vgfx3d_metal_has_complete_splat(cmd->has_splat,
                                                                 cmd->splat_map != NULL,
                                                                 cmd->splat_layers[0] != NULL,
                                                                 cmd->splat_layers[1] != NULL,
                                                                 cmd->splat_layers[2] != NULL,
                                                                 cmd->splat_layers[3] != NULL);
        memset(&mat, 0, sizeof(mat));
        memcpy(mat.dc, cmd->diffuse_color, sizeof(float) * 4);
        mat.sc[0] = cmd->specular[0];
        mat.sc[1] = cmd->specular[1];
        mat.sc[2] = cmd->specular[2];
        mat.sc[3] = cmd->shininess;
        mat.ec[0] = cmd->emissive_color[0];
        mat.ec[1] = cmd->emissive_color[1];
        mat.ec[2] = cmd->emissive_color[2];
        mat.ec[3] = 0;
        mat.scalars[0] = cmd->alpha;
        mat.scalars[1] = cmd->reflectivity;
        mat.scalars[2] = ibl_env_max_lod;
        mat.scalars[3] = ibl_env_lod_base;
        mat.pbrScalars0[0] = cmd->metallic;
        mat.pbrScalars0[1] = cmd->roughness;
        mat.pbrScalars0[2] = cmd->ao;
        mat.pbrScalars0[3] = cmd->emissive_intensity;
        mat.pbrScalars1[0] = cmd->normal_scale;
        mat.pbrScalars1[1] = cmd->alpha_cutoff;
        mat.pbrScalars1[2] = ctx->_opaqueDepthValid ? cmd->soft_particle_fade : 0.0f;
        mat.pbrScalars1[3] = vgfx3d_draw_cmd_ssr_mask(cmd);
        mat.flags0[0] = (cmd->texture || cmd->texture_asset) ? 1 : 0;
        mat.flags0[1] = cmd->unlit;
        mat.flags0[2] = (cmd->normal_map || cmd->normal_map_asset) ? 1 : 0;
        mat.flags0[3] = (cmd->specular_map || cmd->specular_map_asset) ? 1 : 0;
        mat.flags1[0] = (cmd->emissive_map || cmd->emissive_map_asset) ? 1 : 0;
        mat.flags1[1] = (cmd->env_map && (cmd->reflectivity > 0.0001f || ibl_draw)) ? 1 : 0;
        mat.flags1[2] = has_complete_splat;
        mat.flags1[3] = ibl_draw;
        mat.pbrFlags[0] = cmd->workflow;
        mat.pbrFlags[1] = cmd->alpha_mode;
        mat.pbrFlags[2] =
            (cmd->metallic_roughness_map || cmd->metallic_roughness_map_asset) ? 1 : 0;
        mat.pbrFlags[3] = (cmd->ao_map || cmd->ao_map_asset) ? 1 : 0;
        if (has_complete_splat) {
            for (int si = 0; si < 4; si++)
                mat.splatScales[si] = cmd->splat_layer_scales[si];
        }
        mat.shadingModel = cmd->shading_model;
        memcpy(mat.customParams, cmd->custom_params, sizeof(float) * 12);
        for (int slot = 0; slot < RT_MATERIAL3D_TEXTURE_SLOT_COUNT; slot++) {
            if (slot < 4)
                mat.textureUvSets0[slot] = cmd->texture_slot_uv_set[slot];
            else
                mat.textureUvSets1[slot - 4] = cmd->texture_slot_uv_set[slot];
            mat.textureUvTransform0[slot][0] = cmd->texture_slot_uv_transform[slot][0];
            mat.textureUvTransform0[slot][1] = cmd->texture_slot_uv_transform[slot][1];
            mat.textureUvTransform0[slot][2] = cmd->texture_slot_uv_transform[slot][2];
            mat.textureUvTransform0[slot][3] = cmd->texture_slot_uv_transform[slot][3];
            mat.textureUvTransform1[slot][0] = cmd->texture_slot_uv_transform[slot][4];
            mat.textureUvTransform1[slot][1] = cmd->texture_slot_uv_transform[slot][5];
        }
        [ctx.encoder setFragmentBytes:&mat length:sizeof(mat) atIndex:1];

        /* Bind default textures to all shader slots. */
        for (int slot = 0; slot <= 12; slot++)
            [ctx.encoder setFragmentTexture:ctx.defaultTexture atIndex:slot];
        [ctx.encoder setFragmentTexture:ctx.defaultTexture atIndex:14];
        [ctx.encoder setFragmentTexture:ctx.defaultTexture atIndex:15];
        {
            id<MTLTexture> opaqueDepth =
                ctx->_opaqueDepthValid ? ctx.opaqueDepthTexture : metal_opaque_depth_dummy(ctx);
            if (!opaqueDepth)
                return;
            [ctx.encoder setFragmentTexture:opaqueDepth atIndex:16];
        }
        [ctx.encoder setFragmentTexture:ctx.defaultCubemap atIndex:13];
        [ctx.encoder setFragmentTexture:ctx.brdfLutTexture atIndex:18];
        for (int32_t slot = 0; slot < ctx->_shadowCount && slot < VGFX3D_CSM_SLOTS; slot++) {
            if (ctx->_shadowDepthTexture[slot])
                [ctx.encoder setFragmentTexture:ctx->_shadowDepthTexture[slot] atIndex:4 + slot];
        }
        {
            /* Atlas tiles for slots >= VGFX3D_CSM_SLOTS; depth dummy keeps the
             * binding valid when no atlas pass ran this frame. */
            id<MTLTexture> atlas =
                ctx->_shadowAtlasTexture ? ctx->_shadowAtlasTexture : metal_opaque_depth_dummy(ctx);
            if (!atlas)
                return;
            [ctx.encoder setFragmentTexture:atlas atIndex:17];
        }
        [ctx.encoder setFragmentSamplerState:metal_get_material_sampler(
                                                 ctx, cmd, RT_MATERIAL3D_TEXTURE_SLOT_BASE_COLOR)
                                     atIndex:0];
        if (ctx.shadowSampler)
            [ctx.encoder setFragmentSamplerState:ctx.shadowSampler atIndex:1];
        if (ctx.cubeSampler)
            [ctx.encoder setFragmentSamplerState:ctx.cubeSampler atIndex:2];
        [ctx.encoder setFragmentSamplerState:metal_get_material_sampler(
                                                 ctx, cmd, RT_MATERIAL3D_TEXTURE_SLOT_NORMAL)
                                     atIndex:3];
        [ctx.encoder setFragmentSamplerState:metal_get_material_sampler(
                                                 ctx, cmd, RT_MATERIAL3D_TEXTURE_SLOT_SPECULAR)
                                     atIndex:4];
        [ctx.encoder setFragmentSamplerState:metal_get_material_sampler(
                                                 ctx, cmd, RT_MATERIAL3D_TEXTURE_SLOT_EMISSIVE)
                                     atIndex:5];
        [ctx.encoder
            setFragmentSamplerState:metal_get_material_sampler(
                                        ctx, cmd, RT_MATERIAL3D_TEXTURE_SLOT_METALLIC_ROUGHNESS)
                            atIndex:6];
        [ctx.encoder setFragmentSamplerState:metal_get_material_sampler(
                                                 ctx, cmd, RT_MATERIAL3D_TEXTURE_SLOT_AO)
                                     atIndex:7];

        /* MTL-03: Bind cached textures for each material map slot */
        if (cmd->texture || cmd->texture_asset) {
            id<MTLTexture> tex = metal_get_material_texture(
                ctx,
                cmd->texture_asset,
                cmd->texture,
                cmd->texture_asset_cache_key[RT_MATERIAL3D_TEXTURE_SLOT_BASE_COLOR],
                cmd->texture_asset_mip_start[RT_MATERIAL3D_TEXTURE_SLOT_BASE_COLOR],
                cmd->texture_asset_mip_count[RT_MATERIAL3D_TEXTURE_SLOT_BASE_COLOR]);
            if (tex)
                [ctx.encoder setFragmentTexture:tex atIndex:0];
        }
        if (cmd->normal_map || cmd->normal_map_asset) {
            id<MTLTexture> tex = metal_get_material_texture(
                ctx,
                cmd->normal_map_asset,
                cmd->normal_map,
                cmd->texture_asset_cache_key[RT_MATERIAL3D_TEXTURE_SLOT_NORMAL],
                cmd->texture_asset_mip_start[RT_MATERIAL3D_TEXTURE_SLOT_NORMAL],
                cmd->texture_asset_mip_count[RT_MATERIAL3D_TEXTURE_SLOT_NORMAL]);
            if (tex)
                [ctx.encoder setFragmentTexture:tex atIndex:1];
        }
        if (cmd->specular_map || cmd->specular_map_asset) {
            id<MTLTexture> tex = metal_get_material_texture(
                ctx,
                cmd->specular_map_asset,
                cmd->specular_map,
                cmd->texture_asset_cache_key[RT_MATERIAL3D_TEXTURE_SLOT_SPECULAR],
                cmd->texture_asset_mip_start[RT_MATERIAL3D_TEXTURE_SLOT_SPECULAR],
                cmd->texture_asset_mip_count[RT_MATERIAL3D_TEXTURE_SLOT_SPECULAR]);
            if (tex)
                [ctx.encoder setFragmentTexture:tex atIndex:2];
        }
        if (cmd->emissive_map || cmd->emissive_map_asset) {
            id<MTLTexture> tex = metal_get_material_texture(
                ctx,
                cmd->emissive_map_asset,
                cmd->emissive_map,
                cmd->texture_asset_cache_key[RT_MATERIAL3D_TEXTURE_SLOT_EMISSIVE],
                cmd->texture_asset_mip_start[RT_MATERIAL3D_TEXTURE_SLOT_EMISSIVE],
                cmd->texture_asset_mip_count[RT_MATERIAL3D_TEXTURE_SLOT_EMISSIVE]);
            if (tex)
                [ctx.encoder setFragmentTexture:tex atIndex:3];
        }
        if (cmd->metallic_roughness_map || cmd->metallic_roughness_map_asset) {
            id<MTLTexture> tex = metal_get_material_texture(
                ctx,
                cmd->metallic_roughness_map_asset,
                cmd->metallic_roughness_map,
                cmd->texture_asset_cache_key[RT_MATERIAL3D_TEXTURE_SLOT_METALLIC_ROUGHNESS],
                cmd->texture_asset_mip_start[RT_MATERIAL3D_TEXTURE_SLOT_METALLIC_ROUGHNESS],
                cmd->texture_asset_mip_count[RT_MATERIAL3D_TEXTURE_SLOT_METALLIC_ROUGHNESS]);
            if (tex)
                [ctx.encoder setFragmentTexture:tex atIndex:14];
        }
        if (cmd->ao_map || cmd->ao_map_asset) {
            id<MTLTexture> tex = metal_get_material_texture(
                ctx,
                cmd->ao_map_asset,
                cmd->ao_map,
                cmd->texture_asset_cache_key[RT_MATERIAL3D_TEXTURE_SLOT_AO],
                cmd->texture_asset_mip_start[RT_MATERIAL3D_TEXTURE_SLOT_AO],
                cmd->texture_asset_mip_count[RT_MATERIAL3D_TEXTURE_SLOT_AO]);
            if (tex)
                [ctx.encoder setFragmentTexture:tex atIndex:15];
        }
        /* MTL-14: Terrain splat textures (slots 8-12) */
        if (has_complete_splat) {
            id<MTLTexture> sp = metal_get_cached_texture(ctx, cmd->splat_map);
            if (sp)
                [ctx.encoder setFragmentTexture:sp atIndex:8];
            for (int si = 0; si < 4; si++) {
                if (cmd->splat_layers[si]) {
                    id<MTLTexture> lt = metal_get_cached_texture(ctx, cmd->splat_layers[si]);
                    if (lt)
                        [ctx.encoder setFragmentTexture:lt atIndex:9 + si];
                }
            }
        }
        if (env_texture && (cmd->reflectivity > 0.0001f || ibl_draw))
            [ctx.encoder setFragmentTexture:env_texture atIndex:13];

        /* Lights — always set buffer 2, even if empty (prevents validation warnings) */
        if (lights_dirty) {
            mtl_light_t ml[VGFX3D_MAX_LIGHTS];
            memset(ml, 0, sizeof(ml));
            for (int32_t i = 0; i < light_count && i < VGFX3D_MAX_LIGHTS; i++) {
                ml[i].type = lights[i].type;
                ml[i].shadow_index =
                    vgfx3d_metal_sanitize_shadow_index(lights[i].shadow_index, ctx->_shadowCount);
                ml[i].shadow_cascade_count =
                    ml[i].shadow_index >= 0 ? lights[i].shadow_cascade_count : 1;
                ml[i].shadow_projection_type = ml[i].shadow_index >= 0
                                                   ? lights[i].shadow_projection_type
                                                   : VGFX3D_SHADOW_PROJECTION_ORTHOGRAPHIC;
                ml[i].dir[0] = lights[i].direction[0];
                ml[i].dir[1] = lights[i].direction[1];
                ml[i].dir[2] = lights[i].direction[2];
                ml[i].pos[0] = lights[i].position[0];
                ml[i].pos[1] = lights[i].position[1];
                ml[i].pos[2] = lights[i].position[2];
                ml[i].col[0] = lights[i].color[0];
                ml[i].col[1] = lights[i].color[1];
                ml[i].col[2] = lights[i].color[2];
                ml[i].intensity = lights[i].intensity;
                ml[i].attenuation = lights[i].attenuation;
                ml[i].inner_cos = lights[i].inner_cos;
                ml[i].outer_cos = lights[i].outer_cos;
                memcpy(ml[i].shadow_cascade_splits,
                       lights[i].shadow_cascade_splits,
                       sizeof(ml[i].shadow_cascade_splits));
                memcpy(ml[i].basis_u, lights[i].basis_u, sizeof(lights[i].basis_u));
                memcpy(ml[i].basis_v, lights[i].basis_v, sizeof(lights[i].basis_v));
                ml[i].shape[0] = lights[i].width;
                ml[i].shape[1] = lights[i].height;
                ml[i].shape[2] = lights[i].radius;
                ml[i].shape[3] = lights[i].range;
                ml[i].decay_type = lights[i].decay_type;
            }
            int32_t buf_count =
                light_count > 0
                    ? (light_count > VGFX3D_MAX_LIGHTS ? VGFX3D_MAX_LIGHTS : light_count)
                    : 1;
            [ctx.encoder setFragmentBytes:ml length:sizeof(mtl_light_t) * buf_count atIndex:2];
            if (!clusterBuf)
                clusterBuf = metal_cluster_dummy_buffer(ctx);
            if (!clusterBuf)
                return;
            [ctx.encoder setFragmentBuffer:clusterBuf offset:0 atIndex:3];
            ctx->_encoderLightsRevision = cmd->lights_revision;
            ctx->_encoderAmbient[0] = ambient[0];
            ctx->_encoderAmbient[1] = ambient[1];
            ctx->_encoderAmbient[2] = ambient[2];
        }

        [ctx.encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                                indexCount:index_count
                                 indexType:MTLIndexTypeUInt32
                               indexBuffer:ib
                         indexBufferOffset:ib_offset];
        ctx->_stats.draw_calls++;
    }
}

/// @brief End the frame: close the scene encoder, run GPU post-FX, present, and commit.
/// @param ctx_ptr Borrowed `VGFXMetalContext` whose active frame is to be finalized.
static void metal_end_frame(void *ctx_ptr) {
    @autoreleasepool {
        VGFXMetalContext *ctx = (__bridge VGFXMetalContext *)ctx_ptr;
        VGFXMetalRenderTargetCacheEntry *rt_entry = nil;
        id<MTLCommandBuffer> submitted = nil;
        if (!ctx || !ctx.encoder || !ctx.inFrame)
            return;

        [ctx.encoder endEncoding];
        /* Nil immediately: metal_commit_pending below re-runs metal_finish_encoding,
         * which would double-end a stale encoder reference. */
        ctx.encoder = nil;

        if (ctx.rttActive && ctx.rttTarget && ctx.rttColorTexture) {
            rt_entry = metal_lookup_render_target_entry(ctx, ctx.rttTarget);
            submitted = ctx.cmdBuf;
            if (rt_entry)
                rt_entry.pendingCommandBuffer = submitted;
            metal_commit_pending(ctx, NO);
            ctx.rttTarget->sync_color = metal_sync_render_target_color;
            ctx.rttTarget->sync_color_userdata = (__bridge void *)ctx;
            ctx.rttTarget->color_dirty = 1;
            ctx.rttTarget->hdr_color_valid = 0;
        }

        ctx.encoder = nil;
        ctx.inFrame = NO;
    }
}

/// @brief Read the total bytes uploaded to Metal textures so far this frame (diagnostics counter).
/// @param ctx_ptr Borrowed `VGFXMetalContext`; may be NULL.
/// @return The context's accumulated texture-upload byte count, or zero for a NULL context.
static uint64_t metal_get_texture_upload_bytes(void *ctx_ptr) {
    VGFXMetalContext *ctx = (__bridge VGFXMetalContext *)ctx_ptr;
    return ctx ? ctx.textureUploadBytes : 0;
}

/// @brief Present the most recent on-screen drawable. Called from Flip().
/// Commits the frame's command buffer (which contains all render passes)
/// and presents the drawable to the display in one atomic operation.
/// @param backend_ctx Borrowed `VGFXMetalContext` backend handle.
static void metal_present(void *backend_ctx) {
    if (!backend_ctx)
        return;
    @autoreleasepool {
        VGFXMetalContext *ctx = (__bridge VGFXMetalContext *)backend_ctx;
        id<MTLTexture> final_texture = nil;
        vgfx3d_postfx_chain_t stored_chain = {0};
        const vgfx3d_postfx_chain_t *chain = NULL;

        if (!ctx || ctx.rttActive)
            return;

        if (!metal_window_scene_route_enabled(ctx) &&
            ctx.currentTargetKind == VGFX3D_METAL_TARGET_SWAPCHAIN && ctx.cmdBuf && ctx.drawable) {
            if (ctx.captureAfterPresent) {
                /* A presented drawable's contents are undefined; blit it
                 * into the readable display texture first so post-present
                 * readback (SetCaptureAfterPresent contract) sees the
                 * shown frame. */
                (void)metal_capture_current_drawable_to_display_texture(ctx);
            } else {
                ctx.displayTexture = ctx.drawable.texture;
            }
            [ctx.cmdBuf presentDrawable:ctx.drawable];
            metal_commit_pending(ctx, NO);
            ctx->_stats.direct_presents++;
            ctx->_stats.present_path = 1;
            return;
        }
        if (ctx.postfxCompositedToDrawable && ctx.cmdBuf && ctx.drawable) {
            (void)metal_capture_current_drawable_to_display_texture(ctx);
            [ctx.cmdBuf presentDrawable:ctx.drawable];
            metal_commit_pending(ctx, NO);
            ctx.postfxCompositedToDrawable = 0;
            ctx->_stats.direct_presents++;
            ctx->_stats.present_path = 1;
            return;
        }

        if (metal_window_scene_route_enabled(ctx)) {
            if (ctx.gpuPostfxEnabled && ctx.gpuPostfxChainValid) {
                stored_chain = ctx.gpuPostfxChain;
                chain = &stored_chain;
            }
            final_texture = metal_encode_postfx_if_needed(ctx, chain);
        }
        if (!final_texture)
            final_texture = metal_active_readback_texture(ctx);
        if (!final_texture)
            final_texture = ctx.offscreenColor;
        if (metal_present_texture(ctx, final_texture, YES))
            metal_commit_pending(ctx, NO);
        else {
            metal_commit_pending(ctx, YES);
            metal_present_texture_to_framebuffer(ctx, final_texture);
        }
        ctx->_stats.offscreen_presents++;
        ctx->_stats.present_path = 2;
    }
}

/// @brief Handle a window resize: update the layer size and recreate the main render targets.
/// @param ctx_ptr Borrowed `VGFXMetalContext` whose output resources must be resized.
/// @param w New logical output width in pixels; non-positive values are ignored.
/// @param h New logical output height in pixels; non-positive values are ignored.
static void metal_resize(void *ctx_ptr, int32_t w, int32_t h) {
    if (!ctx_ptr || w <= 0 || h <= 0)
        return;
    @autoreleasepool {
        VGFXMetalContext *ctx = (__bridge VGFXMetalContext *)ctx_ptr;
        int32_t prior_width;
        int32_t prior_height;
        if (!ctx || (ctx.width == w && ctx.height == h))
            return;
        metal_commit_pending(ctx, YES);
        prior_width = ctx.width;
        prior_height = ctx.height;
        if (!metal_recreate_main_targets(ctx, w, h)) {
            ctx.width = prior_width;
            ctx.height = prior_height;
            return;
        }
        ctx.width = w;
        ctx.height = h;
        metal_update_layer_size(ctx);
    }
}

/// @brief Read the current frame's rendered image back into a CPU RGBA buffer (for screenshots).
/// @param ctx_ptr Borrowed `VGFXMetalContext` providing the current readable texture.
/// @param[out] dst_rgba Caller-owned destination buffer for tightly packed RGBA pixels.
/// @param w Requested output width in pixels.
/// @param h Requested output height in pixels.
/// @param stride Destination row stride in bytes; must accommodate at least @p w RGBA pixels.
/// @return 1 on success, 0 if no readable texture is available.
static int metal_readback_rgba(
    void *ctx_ptr, uint8_t *dst_rgba, int32_t w, int32_t h, int32_t stride) {
    if (!ctx_ptr)
        return 0;
    @autoreleasepool {
        VGFXMetalContext *ctx = (__bridge VGFXMetalContext *)ctx_ptr;
        id<MTLTexture> texture = nil;
        vgfx3d_postfx_chain_t chain_copy = {0};
        const vgfx3d_postfx_chain_t *chain = NULL;
        vgfx3d_metal_readback_kind_t readback_kind;
        if (!ctx || !dst_rgba || w <= 0 || h <= 0 || stride <= 0)
            return 0;
        if ((size_t)w > SIZE_MAX / 4u || (size_t)stride < (size_t)w * 4u)
            return 0;

        readback_kind = vgfx3d_metal_choose_readback_kind(metal_window_scene_route_enabled(ctx));
        if (!ctx.rttActive && ctx.cmdBuf) {
            if (ctx.postfxCompositedToDrawable) {
                if (!metal_capture_current_drawable_to_display_texture(ctx))
                    return 0;
                ctx.postfxCompositedToDrawable = 0;
            } else if (readback_kind == VGFX3D_METAL_READBACK_POSTFX_COMPOSITE) {
                if (ctx.gpuPostfxChainValid) {
                    vgfx3d_postfx_chain_t stored_chain = ctx.gpuPostfxChain;
                    if (vgfx3d_postfx_chain_copy(&chain_copy, &stored_chain))
                        chain = &chain_copy;
                }
                texture = metal_encode_postfx_if_needed(ctx, chain);
                if (!texture) {
                    vgfx3d_postfx_chain_free(&chain_copy);
                    return 0;
                }
            } else if (ctx.currentTargetKind == VGFX3D_METAL_TARGET_SWAPCHAIN &&
                       !metal_capture_current_drawable_to_display_texture(ctx)) {
                return 0;
            }
            metal_commit_pending(ctx, YES);
        }
        texture = metal_active_readback_texture(ctx);
        {
            const int ok = metal_copy_texture_to_rgba(ctx, texture, dst_rgba, w, h, stride, NULL);
            vgfx3d_postfx_chain_free(&chain_copy);
            return ok;
        }
    }
}

//=============================================================================
// Exported backend
//=============================================================================

/// @brief Bind or unbind an offscreen render target for Metal GPU rendering.
/// When rt is non-NULL, subsequent begin_frame/submit_draw/end_frame calls render
/// to GPU textures instead of the on-screen CAMetalLayer drawable. When rt is NULL,
/// rendering reverts to the screen. RTT color stays GPU-owned until the runtime
/// explicitly asks for CPU pixels through the lazy sync_color hook.
/// @param ctx_ptr Borrowed `VGFXMetalContext` whose target routing is updated.
/// @param rt Borrowed render target to activate, or NULL to restore on-screen rendering.
static void metal_set_render_target(void *ctx_ptr, vgfx3d_rendertarget_t *rt) {
    @autoreleasepool {
        VGFXMetalContext *ctx = (__bridge VGFXMetalContext *)ctx_ptr;
        VGFXMetalRenderTargetCacheEntry *entry;
        if (!ctx)
            return;

        if (!rt) {
            /* Disable RTT — revert to on-screen rendering */
            metal_detach_render_target(ctx, YES);
            return;
        }
        if (ctx.rttTarget && ctx.rttTarget != rt)
            metal_detach_render_target(ctx, YES);

        entry = metal_ensure_render_target_entry(ctx, rt);
        if (!entry) {
            metal_detach_render_target(ctx, NO);
            return;
        }

        ctx.rttTarget = rt;
        ctx.rttWidth = rt->width;
        ctx.rttHeight = rt->height;
        ctx.rttColorTexture = entry.colorTexture;
        ctx.rttMotionTexture = entry.motionTexture;
        ctx.rttDepthTexture = entry.depthTexture;
        ctx.displayTexture = nil;
        ctx.postfxEncodedThisFrame = 0;
        ctx.postfxCompositedToDrawable = 0;
        ctx.rttActive = YES;
    }
}

//=============================================================================
// MTL-12: Shadow mapping
//=============================================================================

/// @brief Abort an active shadow encoder and resume the scene pass without clearing it.
/// @param ctx Borrowed Metal context whose shadow pass is being left.
static void metal_leave_shadow_pass(VGFXMetalContext *ctx) {
    if (!ctx || ctx->_shadowPassSlot < 0)
        return;
    if (ctx.encoder) {
        [ctx.encoder endEncoding];
        ctx.encoder = nil;
    }
    ctx->_shadowPassSlot = -1;
    ctx->_shadowPassFailed = 0;
    metal_recompute_shadow_count(ctx);
    metal_begin_scene_encoder(ctx, YES, YES);
}

/// @brief Backend vtable entry: begin the shadow-map depth pass for shadow-casting light @p slot,
///   rendering depth from the light's @p light_vp view-projection into a @p w×@p h Depth32Float
///   texture (created or resized as needed).
/// @details The CPU @p depth_buf is ignored — the GPU path stores depth in an MTLTexture. No-op for
///   an out-of-range @p slot, or when the device/shadow pipeline/@p light_vp is unavailable or the
///   dimensions are non-positive.
/// @param ctx_ptr Borrowed `VGFXMetalContext` that owns the shadow resources and encoder.
/// @param slot Shadow-light slot to initialize.
/// @param depth_buf Ignored CPU depth buffer retained for backend-interface compatibility.
/// @param w Requested shadow-map width in pixels.
/// @param h Requested shadow-map height in pixels.
/// @param light_vp Borrowed 4x4 light view-projection matrix.
static void metal_shadow_begin(
    void *ctx_ptr, int32_t slot, float *depth_buf, int32_t w, int32_t h, const float *light_vp) {
    @autoreleasepool {
        (void)depth_buf; /* GPU shadows use MTLTexture, not CPU buffer */
        VGFXMetalContext *ctx = (__bridge VGFXMetalContext *)ctx_ptr;
        if (!ctx)
            return;
        metal_leave_shadow_pass(ctx);
        if (slot < 0 || slot >= VGFX3D_MAX_SHADOW_LIGHTS)
            return;
        if (!ctx.device || !ctx.shadowPipeline || !vgfx3d_shadow_matrix_is_usable(light_vp) ||
            !vgfx3d_metal_is_valid_texture2d_extent(w, h)) {
            return;
        }

        int atlas_slot = slot >= VGFX3D_CSM_SLOTS;
        if (atlas_slot && (w > VGFX3D_METAL_MAX_TEXTURE2D_DIMENSION / 4 ||
                           h > VGFX3D_METAL_MAX_TEXTURE2D_DIMENSION / 2)) {
            return;
        }
        ctx->_shadowComplete[slot] = 0;
        ctx->_shadowPassFailed = 0;
        metal_recompute_shadow_count(ctx);
        if (atlas_slot) {
            /* Atlas tile pass: 4x2 tiles at the per-slot resolution. */
            NSUInteger atlas_w = (NSUInteger)w * 4u;
            NSUInteger atlas_h = (NSUInteger)h * 2u;
            if (!ctx->_shadowAtlasTexture || ctx->_shadowAtlasTexture.width != atlas_w ||
                ctx->_shadowAtlasTexture.height != atlas_h) {
                id<MTLTexture> replacement;
                MTLTextureDescriptor *desc = [MTLTextureDescriptor
                    texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                                                 width:atlas_w
                                                height:atlas_h
                                             mipmapped:NO];
                desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
                desc.storageMode = MTLStorageModePrivate;
                replacement = [ctx.device newTextureWithDescriptor:desc];
                if (!replacement)
                    return;
                ctx->_shadowAtlasTexture = replacement;
                ctx->_shadowAtlasClearedSerial = 0;
                for (int32_t atlas_index = VGFX3D_CSM_SLOTS; atlas_index < VGFX3D_MAX_SHADOW_LIGHTS;
                     atlas_index++) {
                    ctx->_shadowComplete[atlas_index] = 0;
                }
                metal_recompute_shadow_count(ctx);
            }
            if (!ctx->_shadowAtlasTexture)
                return;
        } else {
            /* Create/recreate shadow depth texture if size changed */
            if (!ctx->_shadowDepthTexture[slot] ||
                (int32_t)ctx->_shadowDepthTexture[slot].width != w ||
                (int32_t)ctx->_shadowDepthTexture[slot].height != h) {
                id<MTLTexture> replacement;
                MTLTextureDescriptor *desc = [MTLTextureDescriptor
                    texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                                                 width:(NSUInteger)w
                                                height:(NSUInteger)h
                                             mipmapped:NO];
                desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
                desc.storageMode = MTLStorageModePrivate;
                replacement = [ctx.device newTextureWithDescriptor:desc];
                if (!replacement)
                    return;
                ctx->_shadowDepthTexture[slot] = replacement;
            }
            if (!ctx->_shadowDepthTexture[slot])
                return;
        }

        /* Store light VP for main pass uniform */
        vgfx3d_copy_mat4_finite_or_identity(ctx->_shadowLightVP[slot], light_vp);

        /* Start shadow render pass */
        if (!ctx.cmdBuf)
            ctx.cmdBuf = metal_acquire_command_buffer(ctx);
        if (!ctx.cmdBuf)
            return;
        if (ctx.encoder) {
            [ctx.encoder endEncoding];
            ctx.encoder = nil;
        }

        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        BOOL clear_atlas = NO;
        if (atlas_slot) {
            rp.depthAttachment.texture = ctx->_shadowAtlasTexture;
            /* Clear the whole atlas on its first tile pass each frame, then
             * accumulate tiles with Load so earlier tiles survive. */
            clear_atlas = ctx->_shadowAtlasClearedSerial != ctx.frameSerial;
            rp.depthAttachment.loadAction = clear_atlas ? MTLLoadActionClear : MTLLoadActionLoad;
        } else {
            rp.depthAttachment.texture = ctx->_shadowDepthTexture[slot];
            rp.depthAttachment.loadAction = MTLLoadActionClear;
        }
        rp.depthAttachment.storeAction = MTLStoreActionStore;
        rp.depthAttachment.clearDepth = 1.0;

        ctx.encoder = [ctx.cmdBuf renderCommandEncoderWithDescriptor:rp];
        ctx->_encoderLightsRevision = 0; /* fresh encoder: no constants encoded yet */
        if (!ctx.encoder) {
            ctx->_shadowPassSlot = -1;
            return;
        }
        if (clear_atlas)
            ctx->_shadowAtlasClearedSerial = ctx.frameSerial;
        ctx->_shadowPassSlot = slot;
        [ctx.encoder setRenderPipelineState:ctx.shadowPipeline];
        [ctx.encoder setDepthStencilState:ctx.shadowDepthState];
        MTLViewport vp = {0, 0, (double)w, (double)h, 0.0, 1.0};
        if (atlas_slot) {
            int tile = slot - VGFX3D_CSM_SLOTS;
            vp.originX = (double)((tile % 4) * w);
            vp.originY = (double)((tile / 4) * h);
        }
        [ctx.encoder setViewport:vp];
        [ctx.encoder setFrontFacingWinding:MTLWindingCounterClockwise];
        [ctx.encoder setCullMode:MTLCullModeBack];
        [ctx.encoder setDepthBias:0.0f slopeScale:0.0f clamp:0.0f];
    }
}

/// @brief Encode a draw into the active shadow-map depth pass (depth-only, from the light's view).
/// @param ctx_ptr Borrowed `VGFXMetalContext` with an active shadow encoder.
/// @param cmd Borrowed draw command supplying geometry, transforms, alpha state, and deformation.
static void metal_shadow_draw(void *ctx_ptr, const vgfx3d_draw_cmd_t *cmd) {
    @autoreleasepool {
        VGFXMetalContext *ctx = (__bridge VGFXMetalContext *)ctx_ptr;
        id<MTLBuffer> vb;
        id<MTLBuffer> ib;
        NSUInteger vb_offset = 0;
        NSUInteger ib_offset = 0;
        uint32_t index_count;
        int has_skinning;
        int prev_bone_upload_ok = 0;
        metal_morph_bind_status_t morph_status;
        if (!ctx)
            return;
        if (!ctx.encoder || !cmd || !cmd->vertices || !cmd->indices || cmd->vertex_count == 0 ||
            cmd->index_count == 0 || ctx->_shadowPassSlot < 0 ||
            ctx->_shadowPassSlot >= VGFX3D_MAX_SHADOW_LIGHTS) {
            if (ctx->_shadowPassSlot >= 0)
                ctx->_shadowPassFailed = 1;
            return;
        }
        index_count = vgfx3d_draw_cmd_validated_index_count(cmd);
        if (index_count == 0) {
            ctx->_shadowPassFailed = 1;
            return;
        }
        [ctx.encoder setCullMode:cmd->double_sided ? MTLCullModeNone : MTLCullModeBack];
        if (fabsf(cmd->depth_bias) > 1e-8f || fabsf(cmd->slope_scaled_depth_bias) > 1e-8f)
            [ctx.encoder setDepthBias:vgfx3d_depth_bias_constant_units(cmd->depth_bias)
                           slopeScale:cmd->slope_scaled_depth_bias
                                clamp:0.0f];
        else
            [ctx.encoder setDepthBias:0.0f slopeScale:0.0f clamp:0.0f];

        if (!metal_get_geometry_buffers(ctx, cmd, &vb, &ib, &vb_offset, &ib_offset)) {
            ctx->_shadowPassFailed = 1;
            return;
        }
        /* R20: compact-cached geometry needs the packed-layout shadow pipeline. */
        [ctx.encoder setRenderPipelineState:(cmd->compact_vertex_stream &&
                                             ctx.compactPipelinesReady && ctx.shadowPipelineCompact)
                                                ? ctx.shadowPipelineCompact
                                                : ctx.shadowPipeline];
        [ctx.encoder setVertexBuffer:vb offset:vb_offset atIndex:0];
        if (ctx.frameBuffers) {
            [ctx.frameBuffers addObject:vb];
            [ctx.frameBuffers addObject:ib];
        }

        /* Per-object: transform by light VP */
        mtl_per_object_t obj;
        float safe_model[16];
        memset(&obj, 0, sizeof(obj));
        vgfx3d_copy_mat4_finite_or_identity(safe_model, cmd->model_matrix);
        transpose4x4(safe_model, obj.m);
        memcpy(obj.prev_m, obj.m, sizeof(obj.prev_m));
        float lvp_t[16];
        transpose4x4(ctx->_shadowLightVP[ctx->_shadowPassSlot], lvp_t);
        memcpy(obj.vp, lvp_t, sizeof(float) * 16);
        memcpy(obj.nm, obj.m, sizeof(float) * 16);
        has_skinning = (cmd->bone_palette && cmd->bone_count > 0) ? 1 : 0;
        has_skinning = metal_bind_bone_palettes(ctx, cmd, has_skinning, 0, &prev_bone_upload_ok);
        morph_status = metal_bind_morph_payload(ctx, cmd);
        obj.flags0[0] = has_skinning;
        obj.flags0[2] = morph_status.shape_count;
        obj.flags0[3] = morph_status.shape_count > 0 ? (int32_t)cmd->vertex_count : 0;
        /* Shadow pose must match the main pass: bind influences 5-8 too. */
        obj.flags2[1] = has_skinning ? metal_bind_extra_influences(ctx, cmd) : 0;
        [ctx.encoder setVertexBytes:&obj length:sizeof(obj) atIndex:1];
        {
            mtl_per_material_t mat;
            id<MTLTexture> diffuse_tex = metal_get_material_texture(
                ctx,
                cmd->texture_asset,
                cmd->texture,
                cmd->texture_asset_cache_key[RT_MATERIAL3D_TEXTURE_SLOT_BASE_COLOR],
                cmd->texture_asset_mip_start[RT_MATERIAL3D_TEXTURE_SLOT_BASE_COLOR],
                cmd->texture_asset_mip_count[RT_MATERIAL3D_TEXTURE_SLOT_BASE_COLOR]);
            memset(&mat, 0, sizeof(mat));
            memcpy(mat.dc, cmd->diffuse_color, sizeof(float) * 4);
            mat.scalars[0] = cmd->alpha;
            mat.pbrScalars1[1] = cmd->alpha_cutoff;
            mat.flags0[0] = diffuse_tex ? 1 : 0;
            mat.pbrFlags[1] = cmd->alpha_mode;
            for (int tex_slot = 0; tex_slot < RT_MATERIAL3D_TEXTURE_SLOT_COUNT; tex_slot++) {
                if (tex_slot < 4)
                    mat.textureUvSets0[tex_slot] = cmd->texture_slot_uv_set[tex_slot];
                else
                    mat.textureUvSets1[tex_slot - 4] = cmd->texture_slot_uv_set[tex_slot];
                mat.textureUvTransform0[tex_slot][0] = cmd->texture_slot_uv_transform[tex_slot][0];
                mat.textureUvTransform0[tex_slot][1] = cmd->texture_slot_uv_transform[tex_slot][1];
                mat.textureUvTransform0[tex_slot][2] = cmd->texture_slot_uv_transform[tex_slot][2];
                mat.textureUvTransform0[tex_slot][3] = cmd->texture_slot_uv_transform[tex_slot][3];
                mat.textureUvTransform1[tex_slot][0] = cmd->texture_slot_uv_transform[tex_slot][4];
                mat.textureUvTransform1[tex_slot][1] = cmd->texture_slot_uv_transform[tex_slot][5];
            }
            [ctx.encoder setFragmentBytes:&mat length:sizeof(mat) atIndex:1];
            [ctx.encoder setFragmentTexture:diffuse_tex ? diffuse_tex : ctx.defaultTexture
                                    atIndex:0];
            [ctx.encoder
                setFragmentSamplerState:metal_get_material_sampler(
                                            ctx, cmd, RT_MATERIAL3D_TEXTURE_SLOT_BASE_COLOR)
                                atIndex:0];
        }

        [ctx.encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                                indexCount:index_count
                                 indexType:MTLIndexTypeUInt32
                               indexBuffer:ib
                         indexBufferOffset:ib_offset];
    }
}

/// @brief Finish the shadow-map pass for @p slot, storing its depth texture, light VP, and depth
/// bias.
/// @details Marks the slot complete so the lighting pass can sample it; @p bias offsets depth
///          comparisons to reduce shadow acne.
/// @param ctx_ptr Borrowed `VGFXMetalContext` whose shadow encoder is finalized.
/// @param slot Shadow-light slot expected to own the active pass.
/// @param bias Depth-comparison bias stored for subsequent scene lighting.
static void metal_shadow_end(void *ctx_ptr, int32_t slot, float bias) {
    @autoreleasepool {
        VGFXMetalContext *ctx = (__bridge VGFXMetalContext *)ctx_ptr;
        if (!ctx)
            return;
        if (slot < 0 || slot >= VGFX3D_MAX_SHADOW_LIGHTS || ctx->_shadowPassSlot != slot) {
            metal_leave_shadow_pass(ctx);
            return;
        }
        if (!ctx.encoder) {
            ctx->_shadowComplete[slot] = 0;
            metal_leave_shadow_pass(ctx);
            return;
        }
        [ctx.encoder endEncoding];
        ctx.encoder = nil;
        ctx->_shadowComplete[slot] =
            !ctx->_shadowPassFailed && metal_shadow_slot_texture_ready(ctx, slot) ? 1 : 0;
        ctx->_shadowPassSlot = -1;
        ctx->_shadowPassFailed = 0;
        ctx->_shadowBias = vgfx3d_clamp_float_param(bias, -1000000.0f, 1000000.0f, 0.0f);
        metal_recompute_shadow_count(ctx);
        metal_begin_scene_encoder(ctx, YES, YES);
    }
}

/// @brief Backend `shadow_reuse` op — re-arm a slot from its previous-frame depth texture.
/// @details Only the classic per-slot CSM textures persist untouched across frames; the
///   shared atlas texture is whole-cleared on its first tile pass each frame, so a reused
///   atlas tile would be wiped whenever any other tile re-renders — atlas slots decline
///   reuse and take the normal render path.
/// @param ctx_ptr Borrowed `VGFXMetalContext` containing prior-frame shadow textures.
/// @param slot Classic CSM slot whose persistent texture should be reused.
/// @param depth_buf Ignored CPU depth buffer retained for backend-interface compatibility.
/// @param w Expected width of the persistent shadow texture.
/// @param h Expected height of the persistent shadow texture.
/// @param light_vp Borrowed replacement 4x4 light view-projection matrix.
/// @return 1 when the slot was successfully rearmed, or 0 when reuse is unavailable.
static int8_t metal_shadow_reuse(
    void *ctx_ptr, int32_t slot, float *depth_buf, int32_t w, int32_t h, const float *light_vp) {
    @autoreleasepool {
        VGFXMetalContext *ctx = (__bridge VGFXMetalContext *)ctx_ptr;
        (void)depth_buf;
        if (!ctx || slot < 0 || slot >= VGFX3D_CSM_SLOTS ||
            !vgfx3d_shadow_matrix_is_usable(light_vp) || w <= 0 || h <= 0)
            return 0;
        if (!ctx->_shadowDepthTexture[slot] || (int32_t)ctx->_shadowDepthTexture[slot].width != w ||
            (int32_t)ctx->_shadowDepthTexture[slot].height != h)
            return 0;
        vgfx3d_copy_mat4_finite_or_identity(ctx->_shadowLightVP[slot], light_vp);
        ctx->_shadowComplete[slot] = 1;
        metal_recompute_shadow_count(ctx);
        return 1;
    }
}

//=============================================================================
// MTL-13: Instanced rendering
//=============================================================================

/// @brief Backend vtable entry: draw @p instance_count copies of the mesh in @p cmd in one
///   instanced call.
/// @details Ordinary draws consume one 4x4 transform from @p instance_matrices per instance.
///   Particle commands instead point `cmd->particle_instances` at compact center/right/up/color
///   records and may pass NULL for @p instance_matrices. Both modes are lit by @p lights and
///   @p ambient and honor @p wireframe and @p backface_cull. The function is a no-op outside an
///   active frame, on empty geometry, or with no instances. NULL @p ambient defaults to black and
///   a negative @p light_count is treated as zero.
/// @param ctx_ptr Borrowed `VGFXMetalContext` that owns the active command encoder.
/// @param win Window handle supplied by the backend interface; unused by this Metal path.
/// @param cmd Borrowed mesh, material, particle, animation, and per-draw state.
/// @param instance_matrices Borrowed array of column-major 4x4 transforms, or NULL for particles.
/// @param instance_count Number of instances or compact particle records to submit.
/// @param lights Borrowed array of scene-light snapshots; may be NULL when @p light_count is zero.
/// @param light_count Number of entries available through @p lights.
/// @param ambient Borrowed three-component ambient-light color; NULL is sanitized to black.
/// @param wireframe Nonzero to render triangle edges instead of filled triangles.
/// @param backface_cull Nonzero to cull back-facing triangles.
static void metal_submit_draw_instanced(void *ctx_ptr,
                                        vgfx_window_t win,
                                        const vgfx3d_draw_cmd_t *cmd,
                                        const float *instance_matrices,
                                        int32_t instance_count,
                                        const vgfx3d_light_params_t *lights,
                                        int32_t light_count,
                                        const float *ambient,
                                        int8_t wireframe,
                                        int8_t backface_cull) {
    @autoreleasepool {
        (void)win;
        VGFXMetalContext *ctx = (__bridge VGFXMetalContext *)ctx_ptr;
        vgfx3d_metal_instance_data_t *instance_data;
        const void *instance_upload_data;
        size_t instance_upload_bytes;
        int particle_mode;
        id<MTLBuffer> vb;
        id<MTLBuffer> ib;
        id<MTLTexture> env_texture = nil;
        NSUInteger vb_offset = 0;
        NSUInteger ib_offset = 0;
        uint32_t index_count;
        vgfx3d_metal_blend_mode_t blend_mode;
        vgfx3d_draw_cmd_t safe_cmd;
        vgfx3d_light_params_t safe_lights[VGFX3D_MAX_LIGHTS];
        float safe_ambient[3];
        particle_mode = cmd && cmd->particle_instances != NULL;
        if (!ctx)
            return;
        if (!ctx.encoder || !ctx.inFrame || !cmd || !cmd->vertices || !cmd->indices ||
            cmd->vertex_count == 0 || cmd->index_count == 0 ||
            (!particle_mode && !instance_matrices) || instance_count <= 0) {
            ctx->_stats.dropped_draws++;
            return;
        }
        vgfx3d_sanitize_draw_command(cmd, &safe_cmd);
        cmd = &safe_cmd;
        light_count =
            vgfx3d_sanitize_light_array(lights, light_count, safe_lights, VGFX3D_MAX_LIGHTS);
        for (int32_t light_index = 0; light_index < light_count; light_index++)
            vgfx3d_sanitize_light_shadow_span(&safe_lights[light_index], ctx->_shadowCount);
        lights = light_count > 0 ? safe_lights : NULL;
        vgfx3d_sanitize_ambient_rgb(ambient, safe_ambient);
        ambient = safe_ambient;
        particle_mode = cmd->particle_instances != NULL;
        index_count = vgfx3d_draw_cmd_validated_index_count(cmd);
        if (index_count == 0) {
            ctx->_stats.dropped_draws++;
            return;
        }

        if (particle_mode) {
            if ((size_t)instance_count > SIZE_MAX / sizeof(vgfx3d_particle_instance_t))
                return;
            instance_upload_data = cmd->particle_instances;
            instance_upload_bytes = (size_t)instance_count * sizeof(vgfx3d_particle_instance_t);
        } else {
            if (!metal_ensure_instance_storage(ctx, instance_count))
                return;
            instance_data = (vgfx3d_metal_instance_data_t *)ctx.instanceScratch.mutableBytes;
            if (!instance_data)
                return;
            vgfx3d_metal_fill_instance_data(instance_data,
                                            instance_count,
                                            instance_matrices,
                                            cmd->prev_instance_matrices,
                                            cmd->has_prev_instance_matrices ? 1 : 0);
            instance_upload_data = instance_data;
            instance_upload_bytes = (size_t)instance_count * sizeof(vgfx3d_metal_instance_data_t);
        }
        /* Each instanced batch gets its own slice of the transient ring. Binding one shared
         * buffer at offset 0 and memcpy-ing over it per draw corrupted every batch except the
         * last: the encoder records a reference, so the GPU read whatever the final memcpy
         * left behind — all batches rendered with the last batch's transforms. */
        NSUInteger instance_offset = 0;
        id<MTLBuffer> instance_buffer =
            metal_upload_transient_buffer(ctx,
                                          VGFX3D_METAL_TRANSIENT_INSTANCE,
                                          instance_upload_data,
                                          instance_upload_bytes,
                                          &instance_offset);
        if (!instance_buffer)
            return;

        blend_mode = vgfx3d_metal_choose_blend_mode(cmd);
        [ctx.encoder setRenderPipelineState:particle_mode
                                                ? metal_select_particle_pipeline_state(ctx, cmd)
                                                : metal_select_pipeline_state(ctx, cmd, YES)];
        [ctx.encoder setCullMode:backface_cull ? MTLCullModeBack : MTLCullModeNone];
        [ctx.encoder
            setTriangleFillMode:wireframe ? MTLTriangleFillModeLines : MTLTriangleFillModeFill];
        if (cmd->disable_depth_test)
            [ctx.encoder setDepthStencilState:ctx.depthStateDisabled];
        else if (blend_mode != VGFX3D_METAL_BLEND_OPAQUE)
            [ctx.encoder setDepthStencilState:ctx.depthStateNoWrite];
        else
            [ctx.encoder setDepthStencilState:ctx.depthState];
        /* Scene passes are reversed-Z: flip both bias terms so positive material
         * bias still pushes fragments away from the camera. The shadow pass below
         * keeps the standard convention. */
        if (fabsf(cmd->depth_bias) > 1e-8f || fabsf(cmd->slope_scaled_depth_bias) > 1e-8f)
            [ctx.encoder
                setDepthBias:vgfx3d_depth_bias_constant_units_reversed_z(cmd->depth_bias)
                  slopeScale:vgfx3d_depth_bias_slope_reversed_z(cmd->slope_scaled_depth_bias)
                       clamp:0.0f];
        else
            [ctx.encoder setDepthBias:0.0f slopeScale:0.0f clamp:0.0f];

        /* Vertex/index buffers */
        if (!metal_get_geometry_buffers(ctx, cmd, &vb, &ib, &vb_offset, &ib_offset)) {
            ctx->_stats.dropped_draws++;
            return;
        }
        [ctx.encoder setVertexBuffer:vb offset:vb_offset atIndex:0];
        [ctx.encoder setVertexBuffer:instance_buffer offset:instance_offset atIndex:6];
        if (ctx.frameBuffers) {
            [ctx.frameBuffers addObject:vb];
            [ctx.frameBuffers addObject:ib];
        }

        /* Per-object + per-scene + per-material + lights + textures */
        mtl_per_object_t obj;
        memset(&obj, 0, sizeof(obj));
        float vp_t[16];
        int prev_bone_upload_ok = 0;
        metal_morph_bind_status_t morph_status;
        int has_skinning = (cmd->bone_palette && cmd->bone_count > 0) ? 1 : 0;
        int has_prev_skinning = (has_skinning && cmd->prev_bone_palette) ? 1 : 0;
        has_skinning = metal_bind_bone_palettes(
            ctx, cmd, has_skinning, has_prev_skinning, &prev_bone_upload_ok);
        has_prev_skinning = (has_skinning && has_prev_skinning && prev_bone_upload_ok) ? 1 : 0;
        morph_status = metal_bind_morph_payload(ctx, cmd);
        mat4f_identity(obj.m);
        mat4f_identity(obj.prev_m);
        transpose4x4(ctx->_vp, vp_t);
        memcpy(obj.vp, vp_t, sizeof(float) * 16);
        mat4f_identity(obj.nm);
        obj.flags0[0] = has_skinning;
        obj.flags0[1] = has_prev_skinning;
        obj.flags0[2] = morph_status.shape_count;
        obj.flags0[3] = morph_status.shape_count > 0 ? (int32_t)cmd->vertex_count : 0;
        obj.flags1[0] = 0;
        obj.flags1[1] = morph_status.has_prev_weights;
        obj.flags1[2] = cmd->has_prev_instance_matrices ? 1 : 0;
        obj.flags1[3] = morph_status.has_normal_deltas;
        obj.flags2[0] = cmd->instance_bone_stride;
        obj.flags2[1] = has_skinning ? metal_bind_extra_influences(ctx, cmd) : 0;
        [ctx.encoder setVertexBytes:&obj length:sizeof(obj) atIndex:1];

        /* Image-based lighting resolution for this draw (also used at env bind). */
        float ibl_env_lod_base = 0.0f;
        float ibl_env_max_lod = 0.0f;
        if (cmd->env_map && (cmd->reflectivity > 0.0001f || (cmd->ibl_env && ctx->_iblEnabled))) {
            env_texture = metal_get_cached_cubemap(ctx, (const rt_cubemap3d *)cmd->env_map);
        }
        int ibl_draw = metal_cmd_ibl_draw(ctx, cmd, &ibl_env_lod_base, &ibl_env_max_lod);
        /* Scene + light constants persist in the encoder; re-encode only when
         * the queued light-snapshot revision changes (0 = always encode). */
        int lights_dirty =
            (cmd->lights_revision == 0 || cmd->lights_revision != ctx->_encoderLightsRevision ||
             ambient[0] != ctx->_encoderAmbient[0] || ambient[1] != ctx->_encoderAmbient[1] ||
             ambient[2] != ctx->_encoderAmbient[2]);

        mtl_per_scene_t scene;
        id<MTLBuffer> clusterBuf = nil;
        if (lights_dirty) {
            memset(&scene, 0, sizeof(scene));
            memcpy(scene.cp, ctx->_camPos, sizeof(float) * 3);
            scene.cp[3] = ctx->_camIsOrtho ? 1.0f : 0.0f;
            scene.ac[0] = ambient[0];
            scene.ac[1] = ambient[1];
            scene.ac[2] = ambient[2];
            scene.fc[0] = ctx->_fogColor[0];
            scene.fc[1] = ctx->_fogColor[1];
            scene.fc[2] = ctx->_fogColor[2];
            scene.fc[3] = ctx->_fogEnabled ? 1.0f : 0.0f;
            scene.fog_params[0] = ctx->_fogNear;
            scene.fog_params[1] = ctx->_fogFar;
            scene.fog_params[2] = ctx->_shadowBias;
            memcpy(scene.height_fog, ctx->_heightFog, sizeof(scene.height_fog));
            memcpy(scene.height_fog_sun, ctx->_heightFogSun, sizeof(scene.height_fog_sun));
            memcpy(
                scene.height_fog_sun_dir, ctx->_heightFogSunDir, sizeof(scene.height_fog_sun_dir));
            scene.counts[0] = light_count < 0 ? 0
                                              : (light_count > VGFX3D_MAX_LIGHTS ? VGFX3D_MAX_LIGHTS
                                                                                 : light_count);
            scene.counts[1] = ctx->_shadowCount;
            /* Plan 07: clustered forward+. counts[2] >= 0 selects the clustered loop
             * (value = global dir/ambient prefix length); -1 keeps the flat loop. */
            clusterBuf =
                metal_cluster_buffer_for_table(ctx,
                                               (const vgfx3d_cluster_table_t *)cmd->cluster_table,
                                               cmd->lights_revision,
                                               light_count);
            scene.counts[2] =
                clusterBuf
                    ? ((const vgfx3d_cluster_table_t *)cmd->cluster_table)->global_light_count
                    : -1;
            scene.cluster_params[0] =
                (float)(ctx.rttActive ? ctx.rttWidth
                                      : (ctx.currentTargetKind == VGFX3D_METAL_TARGET_SCENE
                                             ? (int32_t)ctx.offscreenColor.width
                                             : ctx.width));
            scene.cluster_params[1] =
                (float)(ctx.rttActive ? ctx.rttHeight
                                      : (ctx.currentTargetKind == VGFX3D_METAL_TARGET_SCENE
                                             ? (int32_t)ctx.offscreenColor.height
                                             : ctx.height));
            /* Plan 10: spare lanes carry the clip planes for depth linearization. */
            scene.cluster_params[2] = ctx->_camNear;
            scene.cluster_params[3] = ctx->_camFar;
            memcpy(scene.camera_forward, ctx->_camForward, sizeof(float) * 3);
            scene.ibl_params[0] = ctx->_iblIntensity;
            scene.ibl_params[1] = ctx->_shadowSlopeBias;
            scene.ibl_params[2] = ctx->_shadowStrength;
            scene.ibl_params[3] =
                (float)(ctx->_shadowQuality <= 0 ? 4 : (ctx->_shadowQuality == 1 ? 8 : 16));
            for (int sh_k = 0; sh_k < 9; sh_k++) {
                scene.sh[sh_k][0] = ctx->_iblSh[sh_k * 3 + 0];
                scene.sh[sh_k][1] = ctx->_iblSh[sh_k * 3 + 1];
                scene.sh[sh_k][2] = ctx->_iblSh[sh_k * 3 + 2];
            }
            transpose4x4(ctx.frameHistory.draw_prev_vp, scene.prev_vp);
            for (int32_t slot = 0; slot < ctx->_shadowCount && slot < VGFX3D_MAX_SHADOW_LIGHTS;
                 slot++)
                transpose4x4(ctx->_shadowLightVP[slot], scene.shadow_vp[slot]);
            [ctx.encoder setVertexBytes:&scene length:sizeof(scene) atIndex:2];
            [ctx.encoder setFragmentBytes:&scene length:sizeof(scene) atIndex:0];
        }

        mtl_per_material_t mat;
        int has_complete_splat = vgfx3d_metal_has_complete_splat(cmd->has_splat,
                                                                 cmd->splat_map != NULL,
                                                                 cmd->splat_layers[0] != NULL,
                                                                 cmd->splat_layers[1] != NULL,
                                                                 cmd->splat_layers[2] != NULL,
                                                                 cmd->splat_layers[3] != NULL);
        memset(&mat, 0, sizeof(mat));
        memcpy(mat.dc, cmd->diffuse_color, sizeof(float) * 4);
        mat.sc[0] = cmd->specular[0];
        mat.sc[1] = cmd->specular[1];
        mat.sc[2] = cmd->specular[2];
        mat.sc[3] = cmd->shininess;
        mat.ec[0] = cmd->emissive_color[0];
        mat.ec[1] = cmd->emissive_color[1];
        mat.ec[2] = cmd->emissive_color[2];
        mat.scalars[0] = cmd->alpha;
        mat.scalars[1] = cmd->reflectivity;
        mat.scalars[2] = ibl_env_max_lod;
        mat.scalars[3] = ibl_env_lod_base;
        mat.pbrScalars0[0] = cmd->metallic;
        mat.pbrScalars0[1] = cmd->roughness;
        mat.pbrScalars0[2] = cmd->ao;
        mat.pbrScalars0[3] = cmd->emissive_intensity;
        mat.pbrScalars1[0] = cmd->normal_scale;
        mat.pbrScalars1[1] = cmd->alpha_cutoff;
        mat.pbrScalars1[2] = ctx->_opaqueDepthValid ? cmd->soft_particle_fade : 0.0f;
        mat.pbrScalars1[3] = vgfx3d_draw_cmd_ssr_mask(cmd);
        mat.flags0[0] = (cmd->texture || cmd->texture_asset) ? 1 : 0;
        mat.flags0[1] = cmd->unlit;
        mat.flags0[2] = (cmd->normal_map || cmd->normal_map_asset) ? 1 : 0;
        mat.flags0[3] = (cmd->specular_map || cmd->specular_map_asset) ? 1 : 0;
        mat.flags1[0] = (cmd->emissive_map || cmd->emissive_map_asset) ? 1 : 0;
        mat.flags1[1] = (cmd->env_map && (cmd->reflectivity > 0.0001f || ibl_draw)) ? 1 : 0;
        mat.flags1[2] = has_complete_splat;
        mat.flags1[3] = ibl_draw;
        mat.pbrFlags[0] = cmd->workflow;
        mat.pbrFlags[1] = cmd->alpha_mode;
        mat.pbrFlags[2] =
            (cmd->metallic_roughness_map || cmd->metallic_roughness_map_asset) ? 1 : 0;
        mat.pbrFlags[3] = (cmd->ao_map || cmd->ao_map_asset) ? 1 : 0;
        if (has_complete_splat) {
            for (int si = 0; si < 4; si++)
                mat.splatScales[si] = cmd->splat_layer_scales[si];
        }
        mat.shadingModel = cmd->shading_model;
        memcpy(mat.customParams, cmd->custom_params, sizeof(float) * 12);
        for (int slot = 0; slot < RT_MATERIAL3D_TEXTURE_SLOT_COUNT; slot++) {
            if (slot < 4)
                mat.textureUvSets0[slot] = cmd->texture_slot_uv_set[slot];
            else
                mat.textureUvSets1[slot - 4] = cmd->texture_slot_uv_set[slot];
            mat.textureUvTransform0[slot][0] = cmd->texture_slot_uv_transform[slot][0];
            mat.textureUvTransform0[slot][1] = cmd->texture_slot_uv_transform[slot][1];
            mat.textureUvTransform0[slot][2] = cmd->texture_slot_uv_transform[slot][2];
            mat.textureUvTransform0[slot][3] = cmd->texture_slot_uv_transform[slot][3];
            mat.textureUvTransform1[slot][0] = cmd->texture_slot_uv_transform[slot][4];
            mat.textureUvTransform1[slot][1] = cmd->texture_slot_uv_transform[slot][5];
        }
        [ctx.encoder setFragmentBytes:&mat length:sizeof(mat) atIndex:1];

        /* Default textures + sampler */
        for (int slot = 0; slot <= 12; slot++)
            [ctx.encoder setFragmentTexture:ctx.defaultTexture atIndex:slot];
        [ctx.encoder setFragmentTexture:ctx.defaultTexture atIndex:14];
        [ctx.encoder setFragmentTexture:ctx.defaultTexture atIndex:15];
        {
            id<MTLTexture> opaqueDepth =
                ctx->_opaqueDepthValid ? ctx.opaqueDepthTexture : metal_opaque_depth_dummy(ctx);
            if (!opaqueDepth)
                return;
            [ctx.encoder setFragmentTexture:opaqueDepth atIndex:16];
        }
        [ctx.encoder setFragmentTexture:ctx.defaultCubemap atIndex:13];
        [ctx.encoder setFragmentTexture:ctx.brdfLutTexture atIndex:18];
        for (int32_t slot = 0; slot < ctx->_shadowCount && slot < VGFX3D_CSM_SLOTS; slot++) {
            if (ctx->_shadowDepthTexture[slot])
                [ctx.encoder setFragmentTexture:ctx->_shadowDepthTexture[slot] atIndex:4 + slot];
        }
        {
            /* Atlas tiles for slots >= VGFX3D_CSM_SLOTS; depth dummy keeps the
             * binding valid when no atlas pass ran this frame. */
            id<MTLTexture> atlas =
                ctx->_shadowAtlasTexture ? ctx->_shadowAtlasTexture : metal_opaque_depth_dummy(ctx);
            if (!atlas)
                return;
            [ctx.encoder setFragmentTexture:atlas atIndex:17];
        }
        [ctx.encoder setFragmentSamplerState:metal_get_material_sampler(
                                                 ctx, cmd, RT_MATERIAL3D_TEXTURE_SLOT_BASE_COLOR)
                                     atIndex:0];
        if (ctx.shadowSampler)
            [ctx.encoder setFragmentSamplerState:ctx.shadowSampler atIndex:1];
        if (ctx.cubeSampler)
            [ctx.encoder setFragmentSamplerState:ctx.cubeSampler atIndex:2];
        [ctx.encoder setFragmentSamplerState:metal_get_material_sampler(
                                                 ctx, cmd, RT_MATERIAL3D_TEXTURE_SLOT_NORMAL)
                                     atIndex:3];
        [ctx.encoder setFragmentSamplerState:metal_get_material_sampler(
                                                 ctx, cmd, RT_MATERIAL3D_TEXTURE_SLOT_SPECULAR)
                                     atIndex:4];
        [ctx.encoder setFragmentSamplerState:metal_get_material_sampler(
                                                 ctx, cmd, RT_MATERIAL3D_TEXTURE_SLOT_EMISSIVE)
                                     atIndex:5];
        [ctx.encoder
            setFragmentSamplerState:metal_get_material_sampler(
                                        ctx, cmd, RT_MATERIAL3D_TEXTURE_SLOT_METALLIC_ROUGHNESS)
                            atIndex:6];
        [ctx.encoder setFragmentSamplerState:metal_get_material_sampler(
                                                 ctx, cmd, RT_MATERIAL3D_TEXTURE_SLOT_AO)
                                     atIndex:7];
        if (cmd->texture || cmd->texture_asset) {
            id<MTLTexture> tex = metal_get_material_texture(
                ctx,
                cmd->texture_asset,
                cmd->texture,
                cmd->texture_asset_cache_key[RT_MATERIAL3D_TEXTURE_SLOT_BASE_COLOR],
                cmd->texture_asset_mip_start[RT_MATERIAL3D_TEXTURE_SLOT_BASE_COLOR],
                cmd->texture_asset_mip_count[RT_MATERIAL3D_TEXTURE_SLOT_BASE_COLOR]);
            if (tex)
                [ctx.encoder setFragmentTexture:tex atIndex:0];
        }
        if (cmd->normal_map || cmd->normal_map_asset) {
            id<MTLTexture> tex = metal_get_material_texture(
                ctx,
                cmd->normal_map_asset,
                cmd->normal_map,
                cmd->texture_asset_cache_key[RT_MATERIAL3D_TEXTURE_SLOT_NORMAL],
                cmd->texture_asset_mip_start[RT_MATERIAL3D_TEXTURE_SLOT_NORMAL],
                cmd->texture_asset_mip_count[RT_MATERIAL3D_TEXTURE_SLOT_NORMAL]);
            if (tex)
                [ctx.encoder setFragmentTexture:tex atIndex:1];
        }
        if (cmd->specular_map || cmd->specular_map_asset) {
            id<MTLTexture> tex = metal_get_material_texture(
                ctx,
                cmd->specular_map_asset,
                cmd->specular_map,
                cmd->texture_asset_cache_key[RT_MATERIAL3D_TEXTURE_SLOT_SPECULAR],
                cmd->texture_asset_mip_start[RT_MATERIAL3D_TEXTURE_SLOT_SPECULAR],
                cmd->texture_asset_mip_count[RT_MATERIAL3D_TEXTURE_SLOT_SPECULAR]);
            if (tex)
                [ctx.encoder setFragmentTexture:tex atIndex:2];
        }
        if (cmd->emissive_map || cmd->emissive_map_asset) {
            id<MTLTexture> tex = metal_get_material_texture(
                ctx,
                cmd->emissive_map_asset,
                cmd->emissive_map,
                cmd->texture_asset_cache_key[RT_MATERIAL3D_TEXTURE_SLOT_EMISSIVE],
                cmd->texture_asset_mip_start[RT_MATERIAL3D_TEXTURE_SLOT_EMISSIVE],
                cmd->texture_asset_mip_count[RT_MATERIAL3D_TEXTURE_SLOT_EMISSIVE]);
            if (tex)
                [ctx.encoder setFragmentTexture:tex atIndex:3];
        }
        if (cmd->metallic_roughness_map || cmd->metallic_roughness_map_asset) {
            id<MTLTexture> tex = metal_get_material_texture(
                ctx,
                cmd->metallic_roughness_map_asset,
                cmd->metallic_roughness_map,
                cmd->texture_asset_cache_key[RT_MATERIAL3D_TEXTURE_SLOT_METALLIC_ROUGHNESS],
                cmd->texture_asset_mip_start[RT_MATERIAL3D_TEXTURE_SLOT_METALLIC_ROUGHNESS],
                cmd->texture_asset_mip_count[RT_MATERIAL3D_TEXTURE_SLOT_METALLIC_ROUGHNESS]);
            if (tex)
                [ctx.encoder setFragmentTexture:tex atIndex:14];
        }
        if (cmd->ao_map || cmd->ao_map_asset) {
            id<MTLTexture> tex = metal_get_material_texture(
                ctx,
                cmd->ao_map_asset,
                cmd->ao_map,
                cmd->texture_asset_cache_key[RT_MATERIAL3D_TEXTURE_SLOT_AO],
                cmd->texture_asset_mip_start[RT_MATERIAL3D_TEXTURE_SLOT_AO],
                cmd->texture_asset_mip_count[RT_MATERIAL3D_TEXTURE_SLOT_AO]);
            if (tex)
                [ctx.encoder setFragmentTexture:tex atIndex:15];
        }
        if (has_complete_splat) {
            id<MTLTexture> sp = metal_get_cached_texture(ctx, cmd->splat_map);
            if (sp)
                [ctx.encoder setFragmentTexture:sp atIndex:8];
            for (int si = 0; si < 4; si++) {
                if (cmd->splat_layers[si]) {
                    id<MTLTexture> lt = metal_get_cached_texture(ctx, cmd->splat_layers[si]);
                    if (lt)
                        [ctx.encoder setFragmentTexture:lt atIndex:9 + si];
                }
            }
        }
        if (env_texture && (cmd->reflectivity > 0.0001f || ibl_draw))
            [ctx.encoder setFragmentTexture:env_texture atIndex:13];

        /* Lights */
        if (lights_dirty) {
            mtl_light_t ml[VGFX3D_MAX_LIGHTS];
            memset(ml, 0, sizeof(ml));
            for (int32_t i = 0; i < light_count && i < VGFX3D_MAX_LIGHTS; i++) {
                ml[i].type = lights[i].type;
                ml[i].shadow_index =
                    vgfx3d_metal_sanitize_shadow_index(lights[i].shadow_index, ctx->_shadowCount);
                ml[i].shadow_cascade_count =
                    ml[i].shadow_index >= 0 ? lights[i].shadow_cascade_count : 1;
                ml[i].shadow_projection_type = ml[i].shadow_index >= 0
                                                   ? lights[i].shadow_projection_type
                                                   : VGFX3D_SHADOW_PROJECTION_ORTHOGRAPHIC;
                ml[i].dir[0] = lights[i].direction[0];
                ml[i].dir[1] = lights[i].direction[1];
                ml[i].dir[2] = lights[i].direction[2];
                ml[i].pos[0] = lights[i].position[0];
                ml[i].pos[1] = lights[i].position[1];
                ml[i].pos[2] = lights[i].position[2];
                ml[i].col[0] = lights[i].color[0];
                ml[i].col[1] = lights[i].color[1];
                ml[i].col[2] = lights[i].color[2];
                ml[i].intensity = lights[i].intensity;
                ml[i].attenuation = lights[i].attenuation;
                ml[i].inner_cos = lights[i].inner_cos;
                ml[i].outer_cos = lights[i].outer_cos;
                memcpy(ml[i].shadow_cascade_splits,
                       lights[i].shadow_cascade_splits,
                       sizeof(ml[i].shadow_cascade_splits));
                memcpy(ml[i].basis_u, lights[i].basis_u, sizeof(lights[i].basis_u));
                memcpy(ml[i].basis_v, lights[i].basis_v, sizeof(lights[i].basis_v));
                ml[i].shape[0] = lights[i].width;
                ml[i].shape[1] = lights[i].height;
                ml[i].shape[2] = lights[i].radius;
                ml[i].shape[3] = lights[i].range;
                ml[i].decay_type = lights[i].decay_type;
            }
            int32_t bc = light_count > 0
                             ? (light_count > VGFX3D_MAX_LIGHTS ? VGFX3D_MAX_LIGHTS : light_count)
                             : 1;
            [ctx.encoder setFragmentBytes:ml length:sizeof(mtl_light_t) * bc atIndex:2];
            if (!clusterBuf)
                clusterBuf = metal_cluster_dummy_buffer(ctx);
            if (!clusterBuf)
                return;
            [ctx.encoder setFragmentBuffer:clusterBuf offset:0 atIndex:3];
            ctx->_encoderLightsRevision = cmd->lights_revision;
            ctx->_encoderAmbient[0] = ambient[0];
            ctx->_encoderAmbient[1] = ambient[1];
            ctx->_encoderAmbient[2] = ambient[2];
        }

        [ctx.encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                                indexCount:index_count
                                 indexType:MTLIndexTypeUInt32
                               indexBuffer:ib
                         indexBufferOffset:ib_offset
                             instanceCount:(NSUInteger)instance_count];
        ctx->_stats.draw_calls++;
    }
}

//=============================================================================
// MTL-11: Post-processing
//=============================================================================

/* C-side PostFX params struct (must match MSL PostFXParams) */
typedef struct {
    float invViewProjection[16];
    float prevViewProjection[16];
    float cameraPosition[4];
    float invResolution[2];
    int32_t bloomEnabled;
    float bloomThreshold, bloomStrength;
    int32_t bloomPasses;
    int32_t tonemapMode;
    float tonemapExposure;
    int32_t fxaaEnabled;
    int32_t colorGradeEnabled;
    float cgBright, cgContrast, cgSat;
    int32_t vignetteEnabled;
    float vigRadius, vigSoftness;
    /* Extended effects (parity with D3D11) */
    int32_t ssaoEnabled;
    float ssaoRadius, ssaoIntensity;
    int32_t ssaoSamples;
    int32_t dofEnabled;
    float dofFocusDist, dofAperture, dofMaxBlur;
    int32_t motionBlurEnabled;
    float motionBlurIntensity;
    int32_t motionBlurSamples;
    int32_t overlayEnabled;
    /* Plan 05 additions (16-byte tail keeps the MSL constant-buffer size in sync). */
    int32_t sceneIsHdr;
    int32_t tonemapExplicit;
    int32_t bloomTexEnabled;
    int32_t taaPad;
} mtl_postfx_params_t;

/* C-side bloom pass params (must match MSL BloomParams). */
typedef struct {
    float srcInvSize[2];
    float threshold;
    int32_t firstPass;
} mtl_bloom_params_t;

/* C-side TAA resolve params (must match MSL TAAParams). */
typedef struct {
    float invViewProjection[16];
    float prevViewProjection[16];
    float invResolution[2];
    float jitterDelta[2];
    float blend;
    int32_t historyValid;
    float pad0[2];
} mtl_taa_params_t;

/* C-side SSR params (must match MSL SSRParams). */
typedef struct {
    float inv_vp[16];
    float vp[16];
    float cam_pos[4];
    float params0[4]; /* x = intensity, y = max roughness (reserved), z = steps */
    float inv_resolution[2];
    float pad1[2];
} mtl_ssr_params_t;

/// @brief Copy the current drawable's contents into the persistent display texture.
/// @details Lets later passes (and the software-present fallback) read the last presented image.
/// @param ctx Borrowed Metal context providing the drawable, command buffer, and display texture.
/// @return 1 on success, 0 if no drawable is available.
static int metal_capture_current_drawable_to_display_texture(VGFXMetalContext *ctx) {
    NSUInteger copy_w;
    NSUInteger copy_h;
    id<MTLBlitCommandEncoder> blit;

    if (!ctx || !ctx.cmdBuf || !ctx.drawable)
        return 0;
    if (!ctx.displayTexture || ctx.displayTexture.pixelFormat != MTLPixelFormatBGRA8Unorm ||
        (int32_t)ctx.displayTexture.width != ctx.width ||
        (int32_t)ctx.displayTexture.height != ctx.height) {
        id<MTLTexture> replacement = metal_new_color_texture(ctx,
                                                             ctx.width,
                                                             ctx.height,
                                                             MTLPixelFormatBGRA8Unorm,
                                                             MTLTextureUsageShaderRead,
                                                             MTLStorageModeShared,
                                                             NO);
        if (!replacement)
            return 0;
        ctx.displayTexture = replacement;
    }
    if (!ctx.displayTexture)
        return 0;

    copy_w = ctx.drawable.texture.width < ctx.displayTexture.width ? ctx.drawable.texture.width
                                                                   : ctx.displayTexture.width;
    copy_h = ctx.drawable.texture.height < ctx.displayTexture.height ? ctx.drawable.texture.height
                                                                     : ctx.displayTexture.height;
    if (copy_w == 0 || copy_h == 0)
        return 0;
    blit = [ctx.cmdBuf blitCommandEncoder];
    if (!blit)
        return 0;
    [blit copyFromTexture:ctx.drawable.texture
              sourceSlice:0
              sourceLevel:0
             sourceOrigin:MTLOriginMake(0, 0, 0)
               sourceSize:MTLSizeMake(copy_w, copy_h, 1)
                toTexture:ctx.displayTexture
         destinationSlice:0
         destinationLevel:0
        destinationOrigin:MTLOriginMake(0, 0, 0)];
    [blit endEncoding];
    return 1;
}

/// @brief Populate post-processing uniforms for one source texture and effect snapshot.
/// @details Resolution-dependent sampling uses the source extent, not the logical output extent,
///          because the first pass may upscale a reduced scene while later passes consume an
///          already native-sized intermediate.
/// @param ctx Borrowed Metal context supplying temporal matrices and camera state.
/// @param postfx Borrowed effect snapshot to sanitize and encode, or NULL for passthrough values.
/// @param overlay_enabled Nonzero when the overlay texture will be composited by this pass.
/// @param bloom_tex_enabled Nonzero when a prefiltered bloom texture is bound for sampling.
/// @param source_is_hdr Nonzero when the current source is linear RGBA16F scene color.
/// @param source_width Width of the pass's source texture in pixels.
/// @param source_height Height of the pass's source texture in pixels.
/// @param[out] params Destination uniform structure populated for the fragment shader.
static void metal_fill_postfx_params(VGFXMetalContext *ctx,
                                     const vgfx3d_postfx_snapshot_t *postfx,
                                     int overlay_enabled,
                                     int bloom_tex_enabled,
                                     int source_is_hdr,
                                     NSUInteger source_width,
                                     NSUInteger source_height,
                                     mtl_postfx_params_t *params) {
    vgfx3d_postfx_snapshot_t safe_postfx;

    if (!ctx || !params)
        return;
    memset(params, 0, sizeof(*params));
    transpose4x4(ctx.frameHistory.scene_inv_vp, params->invViewProjection);
    transpose4x4(ctx.frameHistory.scene_prev_vp, params->prevViewProjection);
    memcpy(params->cameraPosition, ctx.frameHistory.scene_cam_pos, sizeof(float) * 3);
    params->cameraPosition[3] = 1.0f;
    params->invResolution[0] = source_width > 0 ? 1.0f / (float)source_width : 0.0f;
    params->invResolution[1] = source_height > 0 ? 1.0f / (float)source_height : 0.0f;
    params->overlayEnabled = overlay_enabled ? 1 : 0;
    params->sceneIsHdr = source_is_hdr ? 1 : 0;
    params->bloomTexEnabled = bloom_tex_enabled ? 1 : 0;
    if (!postfx)
        return;
    vgfx3d_sanitize_postfx_snapshot(postfx, &safe_postfx);
    postfx = &safe_postfx;
    params->tonemapExplicit = postfx->tonemap_explicit ? 1 : 0;
    params->bloomEnabled = postfx->bloom_enabled ? 1 : 0;
    params->bloomThreshold = postfx->bloom_threshold;
    params->bloomStrength = postfx->bloom_intensity;
    params->bloomPasses = postfx->bloom_passes;
    params->tonemapMode = (int32_t)postfx->tonemap_mode;
    params->tonemapExposure = postfx->tonemap_exposure;
    params->fxaaEnabled = postfx->fxaa_enabled ? 1 : 0;
    params->colorGradeEnabled = postfx->color_grade_enabled ? 1 : 0;
    params->cgBright = postfx->cg_brightness;
    params->cgContrast = postfx->cg_contrast;
    params->cgSat = postfx->cg_saturation;
    params->vignetteEnabled = postfx->vignette_enabled ? 1 : 0;
    params->vigRadius = postfx->vignette_radius;
    params->vigSoftness = postfx->vignette_softness;
    params->dofEnabled = postfx->dof_enabled ? 1 : 0;
    params->dofFocusDist = postfx->dof_focus_distance;
    params->dofAperture = postfx->dof_aperture;
    params->dofMaxBlur = postfx->dof_max_blur;
    params->ssaoEnabled = postfx->ssao_enabled ? 1 : 0;
    params->ssaoRadius = postfx->ssao_radius;
    params->ssaoIntensity = postfx->ssao_intensity;
    params->ssaoSamples = postfx->ssao_samples;
    params->motionBlurEnabled = postfx->motion_blur_enabled ? 1 : 0;
    params->motionBlurIntensity = postfx->motion_blur_intensity;
    params->motionBlurSamples = postfx->motion_blur_samples;
}

/// @brief Encode one full-screen post-processing pass, sampling @p source into a new output
/// texture.
/// @param ctx Borrowed Metal context providing pipelines, attachments, and the command buffer.
/// @param source_texture Borrowed color texture sampled by the post-processing shader.
/// @param target_texture Borrowed color attachment that receives the processed image.
/// @param postfx Borrowed effect snapshot, or NULL to perform a passthrough/composite pass.
/// @param overlay_enabled Nonzero to composite the context's overlay texture.
/// @param bloom_texture Borrowed prefiltered bloom texture, or nil when bloom is unavailable.
/// @return The pass output texture (nil on failure).
static id<MTLTexture> metal_encode_postfx_pass(VGFXMetalContext *ctx,
                                               id<MTLTexture> source_texture,
                                               id<MTLTexture> target_texture,
                                               const vgfx3d_postfx_snapshot_t *postfx,
                                               int overlay_enabled,
                                               id<MTLTexture> bloom_texture) {
    MTLRenderPassDescriptor *rp;
    id<MTLRenderCommandEncoder> pfxEncoder;
    mtl_postfx_params_t params;
    int bloom_tex_enabled;

    if (!ctx || !ctx.cmdBuf || !ctx.postfxPipeline || !source_texture || !target_texture ||
        !ctx.offscreenMotion || !ctx.depthTexture)
        return nil;

    rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = target_texture;
    rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;

    pfxEncoder = [ctx.cmdBuf renderCommandEncoderWithDescriptor:rp];
    if (!pfxEncoder)
        return nil;

    bloom_tex_enabled = (postfx && postfx->bloom_enabled && bloom_texture) ? 1 : 0;

    [pfxEncoder setRenderPipelineState:ctx.postfxPipeline];
    [pfxEncoder setFragmentTexture:source_texture atIndex:0];
    [pfxEncoder setFragmentTexture:ctx.depthTexture atIndex:1];
    [pfxEncoder setFragmentTexture:ctx.offscreenMotion atIndex:2];
    if (overlay_enabled && ctx.overlayColorTexture)
        [pfxEncoder setFragmentTexture:ctx.overlayColorTexture atIndex:3];
    [pfxEncoder setFragmentTexture:(bloom_tex_enabled ? bloom_texture : ctx.defaultTexture)
                           atIndex:4];
    [pfxEncoder setFragmentSamplerState:(ctx.sharedSampler ? ctx.sharedSampler : ctx.defaultSampler)
                                atIndex:0];

    metal_fill_postfx_params(ctx,
                             postfx,
                             overlay_enabled,
                             bloom_tex_enabled,
                             source_texture.pixelFormat == MTLPixelFormatRGBA16Float,
                             source_texture.width,
                             source_texture.height,
                             &params);
    [pfxEncoder setFragmentBytes:&params length:sizeof(params) atIndex:0];

    [pfxEncoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
    [pfxEncoder endEncoding];
    return target_texture;
}

/// @brief Encode the Plan 05 bloom mip chain from the HDR scene color.
/// @details Pass 0 thresholds + Karis-downsamples the scene into the half-res mip; each
///          further mip is a plain 13-tap downsample. The upsample walk tent-filters each
///          deeper mip and additively accumulates it into the one above, leaving mip 0
///          holding the summed octaves. Returns mip 0 (nil when resources are missing).
/// @param ctx Borrowed Metal context providing bloom textures and pipelines.
/// @param source_texture Current ordered-chain color source.
/// @param threshold Luminance threshold applied by the first bloom downsample pass.
/// @return The accumulated first bloom mip, or nil when encoding prerequisites are unavailable.
static id<MTLTexture> metal_encode_bloom_chain(VGFXMetalContext *ctx,
                                               id<MTLTexture> source_texture,
                                               float threshold) {
    NSUInteger levels;

    if (!ctx || !ctx.cmdBuf || !ctx.bloomDownPipeline || !ctx.bloomUpPipeline || !source_texture ||
        !ctx.bloomMipTextures || ctx.bloomMipTextures.count == 0)
        return nil;
    levels = ctx.bloomMipTextures.count;

    for (NSUInteger i = 0; i < levels; i++) {
        id<MTLTexture> src = (i == 0) ? source_texture : ctx.bloomMipTextures[i - 1];
        id<MTLTexture> dst = ctx.bloomMipTextures[i];
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        id<MTLRenderCommandEncoder> enc;
        mtl_bloom_params_t bp;
        rp.colorAttachments[0].texture = dst;
        rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        enc = [ctx.cmdBuf renderCommandEncoderWithDescriptor:rp];
        if (!enc)
            return nil;
        [enc setRenderPipelineState:ctx.bloomDownPipeline];
        [enc setFragmentTexture:src atIndex:0];
        [enc setFragmentSamplerState:(ctx.sharedSampler ? ctx.sharedSampler : ctx.defaultSampler)
                             atIndex:0];
        bp.srcInvSize[0] = src.width > 0 ? 1.0f / (float)src.width : 0.0f;
        bp.srcInvSize[1] = src.height > 0 ? 1.0f / (float)src.height : 0.0f;
        bp.threshold = threshold;
        bp.firstPass = (i == 0) ? 1 : 0;
        [enc setFragmentBytes:&bp length:sizeof(bp) atIndex:0];
        [enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
        [enc endEncoding];
    }

    for (NSUInteger i = levels; i-- > 1;) {
        id<MTLTexture> src = ctx.bloomMipTextures[i];
        id<MTLTexture> dst = ctx.bloomMipTextures[i - 1];
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        id<MTLRenderCommandEncoder> enc;
        mtl_bloom_params_t bp;
        rp.colorAttachments[0].texture = dst;
        rp.colorAttachments[0].loadAction = MTLLoadActionLoad; /* additive accumulate */
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        enc = [ctx.cmdBuf renderCommandEncoderWithDescriptor:rp];
        if (!enc)
            return nil;
        [enc setRenderPipelineState:ctx.bloomUpPipeline];
        [enc setFragmentTexture:src atIndex:0];
        [enc setFragmentSamplerState:(ctx.sharedSampler ? ctx.sharedSampler : ctx.defaultSampler)
                             atIndex:0];
        bp.srcInvSize[0] = src.width > 0 ? 1.0f / (float)src.width : 0.0f;
        bp.srcInvSize[1] = src.height > 0 ? 1.0f / (float)src.height : 0.0f;
        bp.threshold = 0.0f;
        bp.firstPass = 0;
        [enc setFragmentBytes:&bp length:sizeof(bp) atIndex:0];
        [enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
        [enc endEncoding];
    }
    return ctx.bloomMipTextures[0];
}

/// @brief Encode the Plan 05 TAA resolve: reproject + neighborhood-clamp the ping-pong
///        history against @p source_texture, writing the resolved frame into the other
///        history slot (which becomes the new history). Returns the resolved texture.
/// @param ctx Borrowed Metal context providing temporal history, motion, depth, and matrices.
/// @param source_texture Borrowed current-frame color texture to resolve.
/// @param postfx Borrowed TAA settings snapshot, or NULL to use the default blend.
/// @return The newly resolved history texture, or nil when prerequisites or encoding fail.
static id<MTLTexture> metal_encode_taa_pass(VGFXMetalContext *ctx,
                                            id<MTLTexture> source_texture,
                                            const vgfx3d_postfx_snapshot_t *postfx) {
    MTLRenderPassDescriptor *rp;
    id<MTLRenderCommandEncoder> enc;
    id<MTLTexture> history;
    id<MTLTexture> target;
    mtl_taa_params_t tp;

    if (!ctx || !ctx.cmdBuf || !ctx.taaResolvePipeline || !source_texture || !ctx.taaHistoryA ||
        !ctx.taaHistoryB || !ctx.offscreenMotion || !ctx.depthTexture)
        return nil;

    history = ctx.taaHistoryParity ? ctx.taaHistoryA : ctx.taaHistoryB;
    target = ctx.taaHistoryParity ? ctx.taaHistoryB : ctx.taaHistoryA;

    rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = target;
    rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    enc = [ctx.cmdBuf renderCommandEncoderWithDescriptor:rp];
    if (!enc)
        return nil;

    memset(&tp, 0, sizeof(tp));
    transpose4x4(ctx.frameHistory.scene_inv_vp, tp.invViewProjection);
    transpose4x4(ctx.frameHistory.scene_prev_vp, tp.prevViewProjection);
    tp.invResolution[0] = source_texture.width > 0 ? 1.0f / (float)source_texture.width : 0.0f;
    tp.invResolution[1] = source_texture.height > 0 ? 1.0f / (float)source_texture.height : 0.0f;
    /* Clip-space (Y-up) jitter delta converted to the TEXTURE-space (top-left)
     * convention the motion/velocity path now uses: Y negates. */
    tp.jitterDelta[0] = (ctx->_taaJitterClip[0] - ctx->_taaPrevJitterClip[0]) * 0.5f;
    tp.jitterDelta[1] = -(ctx->_taaJitterClip[1] - ctx->_taaPrevJitterClip[1]) * 0.5f;
    tp.blend = (postfx && postfx->taa_enabled) ? postfx->taa_blend : 0.9f;
    tp.historyValid = (ctx.taaHistoryValid && ctx.frameHistory.scene_history_valid) ? 1 : 0;

    [enc setRenderPipelineState:ctx.taaResolvePipeline];
    [enc setFragmentTexture:source_texture atIndex:0];
    [enc setFragmentTexture:history atIndex:1];
    [enc setFragmentTexture:ctx.offscreenMotion atIndex:2];
    [enc setFragmentTexture:ctx.depthTexture atIndex:3];
    [enc setFragmentSamplerState:(ctx.sharedSampler ? ctx.sharedSampler : ctx.defaultSampler)
                         atIndex:0];
    [enc setFragmentBytes:&tp length:sizeof(tp) atIndex:0];
    [enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
    [enc endEncoding];

    ctx.taaHistoryParity = ctx.taaHistoryParity ? 0 : 1;
    ctx.taaHistoryValid = 1;
    return target;
}

/// @brief Encode the Plan 10 SSR pass: ray-march the opaque depth buffer in screen
///        space for pixels whose motion-target alpha carries an SSR mask, compositing
///        reflected scene color over @p source_texture into @p target_texture.
///        Misses keep the source pixel (its env-map reflection term). Returns the
///        target on success, nil when prerequisites are missing.
/// @param ctx Borrowed Metal context providing depth, motion, camera state, and the SSR pipeline.
/// @param source_texture Borrowed scene color sampled by the SSR pass.
/// @param target_texture Borrowed color attachment that receives the SSR composite.
/// @param postfx Borrowed SSR settings snapshot, or NULL to use fallback values.
/// @return @p target_texture after successful encoding, or nil when prerequisites are unavailable.
static id<MTLTexture> metal_encode_ssr_pass(VGFXMetalContext *ctx,
                                            id<MTLTexture> source_texture,
                                            id<MTLTexture> target_texture,
                                            const vgfx3d_postfx_snapshot_t *postfx) {
    MTLRenderPassDescriptor *rp;
    id<MTLRenderCommandEncoder> enc;
    mtl_ssr_params_t sp;

    if (!ctx || !ctx.cmdBuf || !ctx.ssrPipeline || !source_texture || !target_texture ||
        !ctx.offscreenMotion || !ctx.depthTexture)
        return nil;

    rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = target_texture;
    rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    enc = [ctx.cmdBuf renderCommandEncoderWithDescriptor:rp];
    if (!enc)
        return nil;

    memset(&sp, 0, sizeof(sp));
    transpose4x4(ctx.frameHistory.scene_inv_vp, sp.inv_vp);
    transpose4x4(ctx.frameHistory.scene_vp, sp.vp);
    memcpy(sp.cam_pos, ctx.frameHistory.scene_cam_pos, sizeof(float) * 3);
    sp.params0[0] = postfx ? postfx->ssr_intensity : 0.5f;
    sp.params0[1] = postfx ? postfx->ssr_max_roughness : 0.4f;
    sp.params0[2] = (float)(postfx && postfx->ssr_steps > 0 ? postfx->ssr_steps : 24);
    sp.inv_resolution[0] = source_texture.width > 0 ? 1.0f / (float)source_texture.width : 0.0f;
    sp.inv_resolution[1] = source_texture.height > 0 ? 1.0f / (float)source_texture.height : 0.0f;

    [enc setRenderPipelineState:ctx.ssrPipeline];
    [enc setFragmentTexture:source_texture atIndex:0];
    [enc setFragmentTexture:ctx.depthTexture atIndex:1];
    [enc setFragmentTexture:ctx.offscreenMotion atIndex:2];
    [enc setFragmentSamplerState:(ctx.sharedSampler ? ctx.sharedSampler : ctx.defaultSampler)
                         atIndex:0];
    [enc setFragmentBytes:&sp length:sizeof(sp) atIndex:0];
    [enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
    [enc endEncoding];
    return target_texture;
}

/// @brief Run the post-FX chain on the scene color texture if post-processing is enabled.
/// @details Applies each enabled effect (tonemap, bloom, etc.) in sequence and returns the final
///          texture; returns the unmodified scene texture when post-FX is disabled or empty.
/// @param ctx Borrowed Metal context whose scene and intermediate textures are processed.
/// @param postfx Borrowed ordered effect chain, or NULL to encode the required passthrough pass.
/// @return The final display-ready texture, or nil when required resources or encoding fail.
static id<MTLTexture> metal_encode_postfx_if_needed(VGFXMetalContext *ctx,
                                                    const vgfx3d_postfx_chain_t *postfx) {
    id<MTLTexture> source_texture;
    id<MTLTexture> target_texture;

    if (!ctx)
        return nil;
    if (ctx.postfxEncodedThisFrame && ctx.displayTexture)
        return ctx.displayTexture;
    if (!ctx.cmdBuf || !ctx.postfxPipeline || !ctx.postfxColorTexture ||
        !ctx.postfxScratchTexture || !ctx.offscreenColor || !ctx.offscreenMotion ||
        !ctx.depthTexture) {
        return nil;
    }

    metal_finish_encoding(ctx);

    source_texture = ctx.offscreenColor;
    target_texture = ctx.postfxColorTexture;
    if (vgfx3d_postfx_chain_is_usable(postfx)) {
        for (int32_t i = 0; i < postfx->effect_count; i++) {
            id<MTLTexture> output_texture;
            id<MTLTexture> bloom_texture = nil;
            vgfx3d_postfx_snapshot_t safe_snapshot;
            vgfx3d_sanitize_postfx_snapshot(&postfx->effects[i].snapshot, &safe_snapshot);
            if (safe_snapshot.bloom_enabled &&
                postfx->effects[i].type != (int32_t)VGFX3D_POSTFX_EFFECT_TAA &&
                postfx->effects[i].type != (int32_t)VGFX3D_POSTFX_EFFECT_SSR) {
                bloom_texture =
                    metal_encode_bloom_chain(ctx, source_texture, safe_snapshot.bloom_threshold);
                if (!bloom_texture)
                    return nil;
            }
            if (postfx->effects[i].type == (int32_t)VGFX3D_POSTFX_EFFECT_TAA &&
                safe_snapshot.taa_enabled) {
                /* TAA resolves into its RGBA16F history slot, then a passthrough pass
                 * feeds the resolved frame back into the LDR ping-pong chain. */
                id<MTLTexture> resolved =
                    metal_encode_taa_pass(ctx, source_texture, &safe_snapshot);
                if (!resolved)
                    return nil;
                output_texture =
                    metal_encode_postfx_pass(ctx, resolved, target_texture, NULL, 0, nil);
            } else if (postfx->effects[i].type == (int32_t)VGFX3D_POSTFX_EFFECT_SSR &&
                       safe_snapshot.ssr_enabled) {
                output_texture =
                    metal_encode_ssr_pass(ctx, source_texture, target_texture, &safe_snapshot);
                if (!output_texture)
                    return nil;
            } else {
                output_texture = metal_encode_postfx_pass(
                    ctx, source_texture, target_texture, &safe_snapshot, 0, bloom_texture);
            }
            if (!output_texture)
                return nil;
            source_texture = output_texture;
            target_texture = (source_texture == ctx.postfxColorTexture) ? ctx.postfxScratchTexture
                                                                        : ctx.postfxColorTexture;
        }
    } else {
        id<MTLTexture> output_texture =
            metal_encode_postfx_pass(ctx, source_texture, target_texture, NULL, 0, nil);
        if (!output_texture)
            return nil;
        source_texture = output_texture;
        target_texture = ctx.postfxScratchTexture;
    }

    if (ctx.frameHistory.overlay_used_this_frame && ctx.overlayColorTexture) {
        id<MTLTexture> output_texture =
            metal_encode_postfx_pass(ctx, source_texture, target_texture, NULL, 1, nil);
        if (!output_texture)
            return nil;
        source_texture = output_texture;
    }

    ctx.postfxEncodedThisFrame = 1;
    ctx.displayTexture = source_texture;
    return source_texture;
}

/// @brief Metal PostFX presentation: render fullscreen quad with PostFX shader.
/// Runs the fullscreen post-processing pass into an offscreen texture, then
/// presents that texture through the CAMetalLayer drawable when available.
/// @param backend_ctx Borrowed `VGFXMetalContext` backend handle.
/// @param postfx Borrowed ordered post-processing chain to encode before presentation.
static void metal_present_postfx(void *backend_ctx, const vgfx3d_postfx_chain_t *postfx) {
    if (!backend_ctx || !postfx)
        return;
    @autoreleasepool {
        VGFXMetalContext *ctx = (__bridge VGFXMetalContext *)backend_ctx;
        id<MTLTexture> final_texture;

        if (!ctx)
            return;
        final_texture = metal_encode_postfx_if_needed(ctx, postfx);
        if (!final_texture) {
            metal_present(backend_ctx);
            return;
        }

        if (metal_present_texture(ctx, final_texture, YES)) {
            metal_commit_pending(ctx, NO);
            ctx.postfxCompositedToDrawable = 0;
        } else {
            metal_commit_pending(ctx, YES);
            metal_present_texture_to_framebuffer(ctx, final_texture);
            ctx.postfxCompositedToDrawable = 0;
        }
    }
}

/// @brief Metal `apply_postfx` — composite post-FX to the drawable without committing.
/// @param backend_ctx Borrowed `VGFXMetalContext` backend handle.
/// @param postfx Borrowed ordered post-processing chain to composite.
static void metal_apply_postfx(void *backend_ctx, const vgfx3d_postfx_chain_t *postfx) {
    if (!backend_ctx || !postfx)
        return;
    @autoreleasepool {
        VGFXMetalContext *ctx = (__bridge VGFXMetalContext *)backend_ctx;
        id<MTLTexture> final_texture;

        if (!ctx)
            return;
        final_texture = metal_encode_postfx_if_needed(ctx, postfx);
        if (!final_texture)
            return;
        if (metal_present_texture(ctx, final_texture, NO))
            ctx.postfxCompositedToDrawable = 1;
    }
}

/// @brief Draw the skybox using a full-screen triangle sampling the bound environment cubemap.
/// @param ctx_ptr Borrowed `VGFXMetalContext` with an active scene encoder.
/// @param cubemap_ptr Borrowed runtime cubemap whose cached Metal texture is sampled.
static void metal_draw_skybox(void *ctx_ptr, const void *cubemap_ptr) {
    if (!ctx_ptr || !cubemap_ptr)
        return;
    @autoreleasepool {
        VGFXMetalContext *ctx = (__bridge VGFXMetalContext *)ctx_ptr;
        id<MTLTexture> cubemap = metal_get_cached_cubemap(ctx, (const rt_cubemap3d *)cubemap_ptr);
        id<MTLRenderPipelineState> skybox_pipeline;
        id<MTLRenderPipelineState> default_pipeline;
        mtl_skybox_params_t params;
        float view_rot[16];
        float inv_projection[16];
        float inv_view_rot[16];

        if (!ctx || !ctx.encoder || !cubemap)
            return;
        skybox_pipeline =
            metal_target_uses_hdr_color(ctx) ? ctx.skyboxPipeline : ctx.skyboxColorPipeline;
        default_pipeline =
            metal_target_uses_hdr_color(ctx) ? ctx.pipelineState : ctx.pipelineStateColorOnly;
        if (!skybox_pipeline)
            skybox_pipeline = ctx.skyboxPipeline ? ctx.skyboxPipeline : ctx.skyboxColorPipeline;
        if (!default_pipeline)
            default_pipeline = ctx.pipelineState ? ctx.pipelineState : ctx.pipelineStateColorOnly;
        if (!skybox_pipeline || !default_pipeline)
            return;

        memcpy(view_rot, ctx->_view, sizeof(view_rot));
        view_rot[3] = 0.0f;
        view_rot[7] = 0.0f;
        view_rot[11] = 0.0f;
        view_rot[12] = 0.0f;
        view_rot[13] = 0.0f;
        view_rot[14] = 0.0f;
        view_rot[15] = 1.0f;

        memset(&params, 0, sizeof(params));
        if (vgfx3d_invert_matrix4(ctx->_projection, inv_projection) != 0)
            mat4f_identity(inv_projection);
        transpose4x4(view_rot, inv_view_rot);
        transpose4x4(inv_projection, params.inverseProjection);
        transpose4x4(inv_view_rot, params.inverseViewRotation);
        memcpy(params.cameraForward, ctx->_camForward, sizeof(float) * 3);
        params.cameraForward[3] = ctx->_camIsOrtho ? 1.0f : 0.0f;

        [ctx.encoder setCullMode:MTLCullModeNone];
        [ctx.encoder setDepthStencilState:(ctx.skyboxDepthState ? ctx.skyboxDepthState
                                                                : ctx.depthStateNoWrite)];
        [ctx.encoder setRenderPipelineState:skybox_pipeline];
        [ctx.encoder setVertexBytes:&params length:sizeof(params) atIndex:0];
        [ctx.encoder setFragmentBytes:&params length:sizeof(params) atIndex:0];
        [ctx.encoder setFragmentTexture:cubemap atIndex:0];
        [ctx.encoder setFragmentSamplerState:(ctx.cubeSampler ? ctx.cubeSampler : ctx.sharedSampler)
                                     atIndex:0];
        [ctx.encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [ctx.encoder setRenderPipelineState:default_pipeline];
        [ctx.encoder setDepthStencilState:ctx.depthState];
        [ctx.encoder setCullMode:MTLCullModeBack];
    }
}

/// @brief Enable or disable the GPU post-processing pass for subsequent frames.
/// @param ctx_ptr Borrowed `VGFXMetalContext` whose post-processing route is updated.
/// @param enabled Nonzero to enable GPU post-processing when its pipeline is available.
static void metal_set_gpu_postfx_enabled(void *ctx_ptr, int8_t enabled) {
    @autoreleasepool {
        VGFXMetalContext *ctx = (__bridge VGFXMetalContext *)ctx_ptr;
        int8_t desired_enabled;
        int8_t prior_enabled;
        if (!ctx)
            return;
        desired_enabled = (enabled && ctx.postfxPipeline) ? 1 : 0;
        if (ctx.gpuPostfxEnabled == desired_enabled)
            return;
        prior_enabled = ctx.gpuPostfxEnabled;
        if (ctx.cmdBuf)
            metal_commit_pending(ctx, YES);
        ctx.gpuPostfxEnabled = desired_enabled;
        if (ctx.width > 0 && ctx.height > 0 &&
            !metal_recreate_main_targets(ctx, ctx.width, ctx.height)) {
            ctx.gpuPostfxEnabled = prior_enabled;
            return;
        }
        ctx.postfxEncodedThisFrame = 0;
        ctx.postfxCompositedToDrawable = 0;
        ctx.displayTexture = nil;
        if (!ctx.gpuPostfxEnabled)
            metal_reset_gpu_postfx_chain(ctx);
    }
}

/// @brief Apply a reduced window-scene render scale to the Metal backend.
/// @details Scale changes are accepted only between frames. Size-dependent scene, overlay,
///          post-processing, and temporal resources are rebuilt immediately from the shared
///          extent contract. A reduced scale requires the full-screen post-FX pipeline because
///          that pipeline performs the explicit upscale into logical-output-sized targets. If
///          any required allocation fails, the prior scale is restored and its resources are
///          reconstructed before failure is reported. Explicit RTT dimensions are unaffected.
/// @param ctx_ptr Borrowed `VGFXMetalContext` backend handle.
/// @param scale Finite requested scale in `[0.25, 1]`; values at least `0.999` normalize to 1.
/// @return 1 when the requested/native scale is active, or 0 when validation, frame state, shader
///         availability, or resource reconstruction prevents the change.
static int8_t metal_set_render_scale(void *ctx_ptr, float scale) {
    @autoreleasepool {
        VGFXMetalContext *ctx = (__bridge VGFXMetalContext *)ctx_ptr;
        float prior_scale;
        float next_scale;
        int32_t expected_w;
        int32_t expected_h;

        if (!ctx || !vgfx3d_compute_scaled_scene_extent(
                        ctx.width, ctx.height, scale, &expected_w, &expected_h)) {
            return 0;
        }
        next_scale = scale >= 0.999f ? 1.0f : scale;
        if (next_scale < 1.0f && !ctx.postfxPipeline)
            return 0;
        if (ctx.inFrame || ctx.encoder || ctx.cmdBuf)
            return 0;

        prior_scale =
            (isfinite(ctx.renderScale) && ctx.renderScale >= 0.25f && ctx.renderScale <= 1.0f)
                ? ctx.renderScale
                : 1.0f;
        if (fabsf(prior_scale - next_scale) <= 1.0e-6f)
            return 1;

        ctx.renderScale = next_scale;
        if (metal_recreate_main_targets(ctx, ctx.width, ctx.height))
            return 1;

        ctx.renderScale = prior_scale;
        return 0;
    }
}

/// @brief Install a post-FX chain snapshot to drive the GPU post-processing pass (deep-copied).
/// @param ctx_ptr Borrowed `VGFXMetalContext` that owns the resulting chain copy.
/// @param postfx Borrowed chain to copy, or NULL to clear the installed chain.
static void metal_set_gpu_postfx_snapshot(void *ctx_ptr, const vgfx3d_postfx_chain_t *postfx) {
    @autoreleasepool {
        VGFXMetalContext *ctx = (__bridge VGFXMetalContext *)ctx_ptr;
        if (!ctx)
            return;
        if (!postfx) {
            metal_reset_gpu_postfx_chain(ctx);
            ctx.taaHistoryValid = 0;
            return;
        }
        if (!metal_copy_gpu_postfx_chain(ctx, postfx)) {
            metal_reset_gpu_postfx_chain(ctx);
            ctx.taaHistoryValid = 0;
            return;
        }
        /* Plan 05: dropping TAA from the chain invalidates temporal history so a later
         * re-enable starts from a fresh accumulation instead of stale frames. */
        if (!metal_chain_has_effect(postfx, (int32_t)VGFX3D_POSTFX_EFFECT_TAA))
            ctx.taaHistoryValid = 0;
    }
}

/* Hide the Metal layer during software RTT or 2D mode */
/// @brief Hide the Metal layer so GPU-rendered frames stop being presented (used during
///   teardown or when switching away from the GPU backend).
/// @param backend_ctx Borrowed `VGFXMetalContext` whose layer should be hidden.
static void metal_hide_gpu_layer(void *backend_ctx) {
    if (!backend_ctx)
        return;
    @autoreleasepool {
        VGFXMetalContext *ctx = (__bridge VGFXMetalContext *)backend_ctx;
        if (ctx.metalLayer)
            ctx.metalLayer.hidden = YES;
    }
}

/// @brief Make the Metal layer visible (attach/unhide it) so GPU-rendered frames are presented.
/// @param backend_ctx Borrowed `VGFXMetalContext` whose layer should be resized and shown.
static void metal_show_gpu_layer(void *backend_ctx) {
    if (!backend_ctx)
        return;
    @autoreleasepool {
        VGFXMetalContext *ctx = (__bridge VGFXMetalContext *)backend_ctx;
        if (ctx.metalLayer) {
            metal_update_layer_size(ctx);
            ctx.metalLayer.hidden = NO;
        }
    }
}

/// @brief Backend hook: capture every presented frame into the readable
///   display texture BEFORE handing the drawable to the display, so
///   screenshot readback after Present() sees the shown image (the
///   swapchain drawable's contents are undefined once presented). One
///   full-screen blit per presented frame while enabled.
/// @param backend_ctx Borrowed `VGFXMetalContext`.
/// @param enabled Nonzero to enable pre-present capture.
static void metal_set_capture_after_present(void *backend_ctx, int8_t enabled) {
    if (!backend_ctx)
        return;
    @autoreleasepool {
        VGFXMetalContext *ctx = (__bridge VGFXMetalContext *)backend_ctx;
        ctx.captureAfterPresent = enabled ? 1 : 0;
    }
}

/// @brief Backend hook: toggle CAMetalLayer display sync (vsync).
/// @param backend_ctx Borrowed `VGFXMetalContext` that owns the presentation layer.
/// @param enabled Nonzero to synchronize presentation to the display refresh.
static void metal_set_vsync(void *backend_ctx, int8_t enabled) {
    if (!backend_ctx)
        return;
    @autoreleasepool {
        VGFXMetalContext *ctx = (__bridge VGFXMetalContext *)backend_ctx;
        if (ctx.metalLayer)
            ctx.metalLayer.displaySyncEnabled = enabled ? YES : NO;
    }
}

/// @brief Backend hook: register one NDC scene-depth probe for this frame.
/// @details Blits are encoded at commit; results publish through the command buffer's
///   completed handler, so reads see the previous frame's depth without stalling.
/// @param backend_ctx Borrowed `VGFXMetalContext` receiving the probe request.
/// @param ndc_x Horizontal normalized-device coordinate, clamped to `[-1, 1]`.
/// @param ndc_y Vertical normalized-device coordinate, clamped to `[-1, 1]`.
/// @return The nonnegative request slot, or -1 when the request cannot be queued.
static int32_t metal_queue_depth_probe(void *backend_ctx, float ndc_x, float ndc_y) {
    if (!backend_ctx)
        return -1;
    @autoreleasepool {
        VGFXMetalContext *ctx = (__bridge VGFXMetalContext *)backend_ctx;
        if (!ctx || ctx->_depthProbeRequestCount >= VGFX3D_DEPTH_PROBE_MAX || !isfinite(ndc_x) ||
            !isfinite(ndc_y))
            return -1;
        int32_t slot = ctx->_depthProbeRequestCount++;
        /* Bound coordinates before Metal's later float-to-NSUInteger conversion.
         * Merely checking finiteness is insufficient: FLT_MAX is finite but is
         * outside the representable integer domain and makes the cast undefined. */
        ctx->_depthProbeRequests[slot][0] = vgfx3d_clamp_float_param(ndc_x, -1.0f, 1.0f, 0.0f);
        ctx->_depthProbeRequests[slot][1] = vgfx3d_clamp_float_param(ndc_y, -1.0f, 1.0f, 0.0f);
        return slot;
    }
}

/// @brief Backend hook: read a probe result captured on a previous completed frame.
/// @param backend_ctx Borrowed `VGFXMetalContext` containing published probe results.
/// @param slot Nonnegative probe slot previously returned by metal_queue_depth_probe().
/// @return The sampled scene depth, or -1 when the context or slot has no available result.
static float metal_read_depth_probe(void *backend_ctx, int32_t slot) {
    if (!backend_ctx || slot < 0)
        return -1.0f;
    @autoreleasepool {
        VGFXMetalContext *ctx = (__bridge VGFXMetalContext *)backend_ctx;
        float value = -1.0f;
        if (!ctx)
            return -1.0f;
        @synchronized(ctx) {
            if (slot < ctx->_depthProbeResultCount)
                value = ctx->_depthProbeResults[slot];
        }
        return value;
    }
}
