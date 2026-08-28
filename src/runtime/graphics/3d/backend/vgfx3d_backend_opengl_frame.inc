//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/backend/vgfx3d_backend_opengl_frame.inc
// Purpose: OpenGL frame lifecycle, draw submission, readback, post-FX, and shadow passes.
// Key invariants:
//   - Submission validates geometry and restores instanced attribute state on every exit path.
//   - Compact particle commands use their dedicated attributes with a null matrix payload.
// Ownership/Lifetime:
//   - GL resources are owned by gl_context_t and remain bound only for the active synchronous call.
//   - Canvas command payloads are borrowed and are never retained beyond backend dispatch.
// Links: vgfx3d_backend_opengl.c, vgfx3d_backend_opengl_mesh.inc
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements OpenGL frame setup, submission, presentation, readback, and shadow hooks.
/// @details This implementation fragment drives the backend vtable's per-frame operations,
///          including target routing, temporal history, asynchronous depth probes, post-processing
///          composition, render-target synchronization, and regular or instanced mesh submission.

/// @brief Move a context off direct presentation after a failed default-framebuffer resolve.
///
/// @details `gl_probe_default_framebuffer_writable` only proves the drawable accepts a clear and
///   a read; some GLX stacks pass that and still discard the fullscreen shader resolve post-FX
///   needs. Rather than presume failure for everyone, presentation starts direct and this demotes
///   the context the first time a composite actually fails. The decision is sticky — a drawable
///   that rejected the resolve once will keep doing so, and re-probing every frame would trade a
///   one-time cost for a permanent one.
/// @param ctx Borrowed OpenGL context to move onto the ZannaGFX offscreen route; NULL is ignored.
static void gl_demote_to_offscreen_present(gl_context_t *ctx) {
    if (!ctx || !ctx->default_framebuffer_writable)
        return;
    ctx->default_framebuffer_writable = 0;
    ctx->present_path = 2;
    ctx->stats.present_path = 2;
    ctx->stats.default_framebuffer_writable = 0;
    if (gl_debug_enabled()) {
        fprintf(stderr,
                "[OpenGL] default framebuffer resolve failed; demoting to offscreen present\n");
    }
}

/// @brief Backend `clear` op: store the clear color for use during `BeginFrame`.
///
/// We don't actually issue `glClear` here; that happens in
/// `BeginFrame` so we know the current target/load-state.
/// @param ctx_ptr Borrowed OpenGL backend context receiving the sanitized clear color.
/// @param win Window handle retained for backend-interface compatibility; unused here.
/// @param r Requested red component.
/// @param g Requested green component.
/// @param b Requested blue component.
static void gl_clear(void *ctx_ptr, vgfx_window_t win, float r, float g, float b) {
    (void)win;
    gl_context_t *ctx = (gl_context_t *)ctx_ptr;
    if (!ctx)
        return;
    ctx->clearR = vgfx3d_clamp_float_param(r, 0.0f, 1.0f, 0.0f);
    ctx->clearG = vgfx3d_clamp_float_param(g, 0.0f, 1.0f, 0.0f);
    ctx->clearB = vgfx3d_clamp_float_param(b, 0.0f, 1.0f, 0.0f);
}

/// @brief Radical-inverse Halton sequence sample for TAA sub-pixel jitter (Plan 05).
/// @param index Positive sequence index.
/// @param base Radical-inverse base, normally two or three for TAA jitter.
/// @return The corresponding sample in `[0, 1)`.
static float gl_halton(uint32_t index, uint32_t base) {
    float f = 1.0f;
    float r = 0.0f;
    while (index > 0) {
        f /= (float)base;
        r += f * (float)(index % base);
        index /= base;
    }
    return r;
}

/// @brief True when the exported postfx chain contains an effect of @p type_value.
/// @param chain Borrowed post-processing chain to inspect.
/// @param type_value Effect-type identifier to locate.
/// @return Nonzero when a usable chain contains the requested effect.
static int gl_chain_has_effect(const vgfx3d_postfx_chain_t *chain, int32_t type_value) {
    if (!vgfx3d_postfx_chain_is_usable(chain))
        return 0;
    for (int32_t i = 0; i < chain->effect_count; i++) {
        if (chain->effects[i].type == type_value)
            return 1;
    }
    return 0;
}

/// @brief Backend `begin_frame` — set up matrices, target, frame-history, and clear.
///
/// Per-frame setup steps:
///   1. Make GLX context current (multi-window apps may have switched).
///   2. Bump frame serial; reset shadow-active flag.
///   3. Detect overlay pass (skips scene composition).
///   4. Prune all caches (texture / cubemap / morph / mesh) to bound residency.
///   5. Compute VP + inverse VP from camera, capture fog state.
///   6. Roll the frame-history (current VP → previous VP, etc.).
///   7. If transitioning from scene to overlay, composite scene → backbuffer.
///   8. Choose active target kind, ensure offscreen targets exist.
///   9. Clear color/depth based on `load_existing_*` flags.
///  10. Reset blend / depth / cull / poly-mode state for clean draws.
/// @param ctx_ptr Borrowed OpenGL backend context to prepare for drawing.
/// @param cam Borrowed camera and pass snapshot; values are sanitized before use.
static void gl_begin_frame(void *ctx_ptr, const vgfx3d_camera_params_t *cam) {
    gl_context_t *ctx = (gl_context_t *)ctx_ptr;
    vgfx3d_opengl_frame_history_t history;
    int32_t scene_width;
    int32_t scene_height;
    vgfx3d_camera_params_t safe_cam;
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
    if (!ctx || !cam)
        return;
    vgfx3d_sanitize_camera_params(cam, &safe_cam);
    cam = &safe_cam;
    if (!vgfx3d_compute_scaled_scene_extent(
            ctx->width, ctx->height, ctx->render_scale, &scene_width, &scene_height)) {
        scene_width = ctx->width;
        scene_height = ctx->height;
    }
    if (!gl_make_current(ctx))
        return;
    ctx->frame_active = 1;

    ctx->frame_serial++;
    /* A new frame invalidates the copy retained at the previous present. */
    ctx->present_capture_valid = 0;
    if (ctx->timer_queries_ready) {
        /* Retire whatever the GPU has finished before claiming a slot, so the ring
         * only ever recycles a slot whose result has already been collected. */
        gl_harvest_timer_queries(ctx);
        ctx->timer_slot_active = ctx->timer_query_head;
        ctx->timer_query_pending[ctx->timer_slot_active] = 0;
        gl.QueryCounter(ctx->timer_query[ctx->timer_slot_active][0], GL_TIMESTAMP);
    }
    /* Plan 07: cluster tables are camera-dependent; a light revision surviving
     * into a new frame does not imply the same froxel contents. */
    ctx->uploaded_cluster_revision = 0;
    /* Plan 10: the opaque depth snapshot is per-pass state. */
    ctx->opaque_depth_valid = 0;
    ctx->cam_znear = cam->znear;
    ctx->cam_zfar = cam->zfar;
    ctx->shadow_pass_slot = -1;
    ctx->shadow_count = 0;
    memset(ctx->shadow_complete, 0, sizeof(ctx->shadow_complete));
    ctx->current_pass_is_overlay = (!ctx->rtt_active && cam->load_existing_color) ? 1 : 0;
    /* Every pass — the overlay included — starts with a fresh upload budget (ADR 0299 note):
     * sharing the scene pass's remainder let a heavy cut pause texture-required HUD quads. */
    ctx->texture_upload_bytes = 0;
    if (!ctx->current_pass_is_overlay) {
        ctx->scene_composited_to_backbuffer = 0;
        ctx->scene_postfx_pending = 0;
        /* Poll the prior depth-probe fence before mapping its PBO. A frame boundary does not
         * guarantee GPU completion; mapping first could stall the render thread or overwriting
         * the same PBO could race an in-flight ReadPixels command. */
        if (ctx->depth_probe_pending_count > 0 && ctx->depth_probe_pbo) {
            GLenum wait_result = ctx->depth_probe_fence
                                     ? gl.ClientWaitSync(ctx->depth_probe_fence, 0, 0)
                                     : GL_WAIT_FAILED;
            if (wait_result == GL_ALREADY_SIGNALED || wait_result == GL_CONDITION_SATISFIED) {
                float results[VGFX3D_DEPTH_PROBE_MAX];
                int32_t result_count = ctx->depth_probe_pending_count;
                const float *mapped;
                gl.DeleteSync(ctx->depth_probe_fence);
                ctx->depth_probe_fence = NULL;
                gl.BindBuffer(GL_PIXEL_PACK_BUFFER, ctx->depth_probe_pbo);
                mapped = (const float *)gl.MapBufferRange(
                    GL_PIXEL_PACK_BUFFER,
                    0,
                    (GLsizeiptr)((size_t)result_count * sizeof(float)),
                    GL_MAP_READ_BIT);
                if (mapped) {
                    for (int32_t i = 0; i < result_count; i++)
                        results[i] = vgfx3d_sanitize_reversed_depth_probe_result(mapped[i]);
                    if (gl.UnmapBuffer(GL_PIXEL_PACK_BUFFER)) {
                        memcpy(ctx->depth_probe_results,
                               results,
                               (size_t)result_count * sizeof(results[0]));
                        ctx->depth_probe_result_count = result_count;
                    } else {
                        ctx->depth_probe_result_count = 0;
                    }
                } else {
                    ctx->depth_probe_result_count = 0;
                }
                gl.BindBuffer(GL_PIXEL_PACK_BUFFER, 0);
                ctx->depth_probe_pending_count = 0;
                ctx->depth_probe_pending_polls = 0;
            } else if (wait_result == GL_TIMEOUT_EXPIRED) {
                if (ctx->depth_probe_pending_polls < UINT32_MAX)
                    ctx->depth_probe_pending_polls++;
                if (ctx->depth_probe_pending_polls >= 120u) {
                    gl.DeleteSync(ctx->depth_probe_fence);
                    ctx->depth_probe_fence = NULL;
                    gl.DeleteBuffers(1, &ctx->depth_probe_pbo);
                    ctx->depth_probe_pbo = 0;
                    ctx->depth_probe_pending_count = 0;
                    ctx->depth_probe_pending_polls = 0;
                    ctx->depth_probe_result_count = 0;
                }
            } else {
                if (ctx->depth_probe_fence)
                    gl.DeleteSync(ctx->depth_probe_fence);
                ctx->depth_probe_fence = NULL;
                gl.DeleteBuffers(1, &ctx->depth_probe_pbo);
                ctx->depth_probe_pbo = 0;
                ctx->depth_probe_pending_count = 0;
                ctx->depth_probe_pending_polls = 0;
                ctx->depth_probe_result_count = 0;
            }
        }
        ctx->depth_probe_request_count = 0;
    }
    texture_cache_prune(ctx);
    cubemap_cache_prune(ctx);
    morph_cache_prune(ctx);
    gl_mesh_cache_prune(ctx);

    vgfx3d_copy_mat4_finite_or_identity(ctx->view, cam->view);
    vgfx3d_copy_mat4_finite_or_identity(ctx->projection, cam->projection);
    mat4f_mul_gl(ctx->projection, ctx->view, ctx->vp);
    /* Plan 05 TAA: apply Halton(2,3) sub-pixel jitter to the scene-pass VP when the
     * latched chain contains a TAA resolve. Injected before inv/prev VP derivation so
     * every consumer this frame sees one consistent (jittered) VP; the resolve pass
     * removes the current-minus-previous jitter delta from velocities. Overlay passes
     * keep the previous state so UI stays fixed and the sequence advances once per
     * frame. */
    if (!ctx->rtt_active && !ctx->current_pass_is_overlay) {
        ctx->taa_prev_jitter_clip[0] = ctx->taa_jitter_clip[0];
        ctx->taa_prev_jitter_clip[1] = ctx->taa_jitter_clip[1];
        ctx->taa_jitter_clip[0] = 0.0f;
        ctx->taa_jitter_clip[1] = 0.0f;
        if (ctx->gpu_postfx_enabled && ctx->gpu_postfx_chain_valid && ctx->taa_program &&
            scene_width > 0 && scene_height > 0 &&
            gl_chain_has_effect(&ctx->gpu_postfx_chain, (int32_t)VGFX3D_POSTFX_EFFECT_TAA)) {
            uint32_t idx = (ctx->taa_frame_index++ & 7u) + 1u;
            float jx = (gl_halton(idx, 2) - 0.5f) * 2.0f / (float)scene_width;
            float jy = (gl_halton(idx, 3) - 0.5f) * 2.0f / (float)scene_height;
            ctx->taa_jitter_clip[0] = jx;
            ctx->taa_jitter_clip[1] = jy;
            /* Row-major clip-space translate: x-row += jx * w-row, y-row += jy * w-row. */
            for (int jc = 0; jc < 4; jc++) {
                ctx->vp[0 * 4 + jc] += jx * ctx->vp[3 * 4 + jc];
                ctx->vp[1 * 4 + jc] += jy * ctx->vp[3 * 4 + jc];
            }
        }
    }
    if (!vgfx3d_float_array_is_bounded(ctx->vp, 16u, VGFX3D_BACKEND_MATRIX_COMPONENT_ABS_MAX) ||
        mat4f_inverse_gl(ctx->vp, ctx->inv_vp) != 0) {
        memcpy(ctx->vp, kIdentity4x4, sizeof(ctx->vp));
        memcpy(ctx->inv_vp, kIdentity4x4, sizeof(ctx->inv_vp));
        ctx->taa_jitter_clip[0] = 0.0f;
        ctx->taa_jitter_clip[1] = 0.0f;
        ctx->taa_prev_jitter_clip[0] = 0.0f;
        ctx->taa_prev_jitter_clip[1] = 0.0f;
        ctx->taa_history_valid = 0;
        ctx->scene_history_valid = 0;
    }
    vgfx3d_copy_float_array_finite_or(ctx->cam_pos, cam->position, 3u, 0.0f);
    vgfx3d_copy_vec3_direction_or(ctx->cam_forward, cam->forward, NULL);
    ctx->cam_is_ortho = cam->is_ortho ? 1 : 0;
    ctx->fog_enabled = cam->fog_enabled;
    ctx->height_fog[0] = cam->height_fog_base;
    ctx->height_fog[1] = cam->height_fog_falloff;
    ctx->height_fog[2] =
        cam->height_fog_enabled ? cam->height_fog_density * cam->height_fog_blend : 0.0f;
    ctx->height_fog[3] = 0.0f;
    ctx->height_fog_sun[0] = cam->height_fog_sun_color[0];
    ctx->height_fog_sun[1] = cam->height_fog_sun_color[1];
    ctx->height_fog_sun[2] = cam->height_fog_sun_color[2];
    ctx->height_fog_sun[3] = cam->height_fog_enabled ? cam->height_fog_sun_amount : 0.0f;
    ctx->height_fog_sun_dir[0] = cam->height_fog_sun_dir[0];
    ctx->height_fog_sun_dir[1] = cam->height_fog_sun_dir[1];
    ctx->height_fog_sun_dir[2] = cam->height_fog_sun_dir[2];
    ctx->height_fog_sun_dir[3] = cam->height_fog_sun_power;
    ctx->fog_near = cam->fog_near;
    ctx->fog_far = cam->fog_far;
    ctx->fog_color[0] = cam->fog_color[0];
    ctx->fog_color[1] = cam->fog_color[1];
    ctx->fog_color[2] = cam->fog_color[2];
    ctx->ibl_enabled = cam->ibl_enabled;
    ctx->ibl_intensity = cam->ibl_intensity;
    memcpy(ctx->ibl_sh, cam->ibl_sh, sizeof(ctx->ibl_sh));
    ctx->shadow_strength = cam->shadow_strength;
    ctx->shadow_slope_bias = cam->shadow_slope_bias;
    ctx->shadow_quality = cam->shadow_quality;
    memcpy(history.scene_vp, ctx->scene_vp, sizeof(history.scene_vp));
    memcpy(history.scene_prev_vp, ctx->scene_prev_vp, sizeof(history.scene_prev_vp));
    memcpy(history.scene_inv_vp, ctx->scene_inv_vp, sizeof(history.scene_inv_vp));
    memcpy(history.draw_prev_vp, ctx->draw_prev_vp, sizeof(history.draw_prev_vp));
    memcpy(history.scene_cam_pos, ctx->scene_cam_pos, sizeof(history.scene_cam_pos));
    history.scene_history_valid = ctx->scene_history_valid;
    vgfx3d_opengl_update_frame_history(
        &history, ctx->vp, ctx->inv_vp, ctx->cam_pos, ctx->current_pass_is_overlay);
    memcpy(ctx->scene_vp, history.scene_vp, sizeof(ctx->scene_vp));
    memcpy(ctx->scene_prev_vp, history.scene_prev_vp, sizeof(ctx->scene_prev_vp));
    memcpy(ctx->scene_inv_vp, history.scene_inv_vp, sizeof(ctx->scene_inv_vp));
    memcpy(ctx->draw_prev_vp, history.draw_prev_vp, sizeof(ctx->draw_prev_vp));
    memcpy(ctx->scene_cam_pos, history.scene_cam_pos, sizeof(ctx->scene_cam_pos));
    ctx->scene_history_valid = history.scene_history_valid;

    if (ctx->current_pass_is_overlay && gl_window_scene_route_enabled(ctx) &&
        ctx->scene_postfx_pending && !ctx->scene_composited_to_backbuffer) {
        if (draw_scene_texture(ctx, ctx->gpu_postfx_chain_valid ? &ctx->gpu_postfx_chain : NULL))
            ctx->scene_composited_to_backbuffer = 1;
    }

    if (ctx->current_pass_is_overlay && gl_window_scene_route_enabled(ctx))
        ctx->active_target_kind = VGFX3D_OPENGL_TARGET_SWAPCHAIN;
    else
        ctx->active_target_kind =
            vgfx3d_opengl_choose_target_kind(ctx->rtt_active, gl_window_scene_route_enabled(ctx));
    if (!ctx->current_pass_is_overlay &&
        ctx->active_target_kind == VGFX3D_OPENGL_TARGET_SWAPCHAIN &&
        !ctx->default_framebuffer_writable) {
        ctx->active_target_kind = VGFX3D_OPENGL_TARGET_SCENE;
    }

    if (ctx->active_target_kind == VGFX3D_OPENGL_TARGET_SCENE) {
        if (ensure_scene_targets(ctx, scene_width, scene_height) != 0)
            ctx->active_target_kind = VGFX3D_OPENGL_TARGET_SWAPCHAIN;
        else
            ctx->scene_postfx_pending = 1;
    } else if (!ctx->current_pass_is_overlay) {
        ctx->scene_postfx_pending = 0;
    }

    bind_main_framebuffer(ctx);

    if (ctx->active_target_kind == VGFX3D_OPENGL_TARGET_RTT) {
        GLbitfield clear_mask = 0;
        if (!cam->load_existing_color) {
            gl.ClearColor(ctx->clearR, ctx->clearG, ctx->clearB, 1.0f);
            clear_mask |= GL_COLOR_BUFFER_BIT;
        }
        if (!cam->load_existing_depth) {
            /* Reversed-Z: 0 is the far plane; scene draws compare Greater. */
            gl.ClearDepth(0.0);
            clear_mask |= GL_DEPTH_BUFFER_BIT;
        }
        if (clear_mask)
            gl.Clear(clear_mask);
    } else if (ctx->active_target_kind == VGFX3D_OPENGL_TARGET_SCENE) {
        if (!cam->load_existing_color) {
            gl.DrawBuffer(GL_COLOR_ATTACHMENT0);
            gl.ClearColor(ctx->clearR, ctx->clearG, ctx->clearB, 1.0f);
            gl.Clear(GL_COLOR_BUFFER_BIT);
            gl.DrawBuffer(GL_COLOR_ATTACHMENT1);
            gl.ClearColor(0.5f, 0.5f, 0.0f, 0.0f); /* alpha = SSR mask; empty = 0 */
            gl.Clear(GL_COLOR_BUFFER_BIT);
        }
        if (!cam->load_existing_depth) {
            gl.DrawBuffer(GL_COLOR_ATTACHMENT0);
            /* Reversed-Z: 0 is the far plane; scene draws compare Greater. */
            gl.ClearDepth(0.0);
            gl.Clear(GL_DEPTH_BUFFER_BIT);
        }
        {
            const GLenum attachments[2] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
            gl.DrawBuffers(2, attachments);
        }
    } else {
        GLbitfield clear_mask = 0;
        if (!cam->load_existing_color) {
            gl.ClearColor(ctx->clearR, ctx->clearG, ctx->clearB, 1.0f);
            clear_mask |= GL_COLOR_BUFFER_BIT;
        }
        if (!cam->load_existing_depth) {
            /* Reversed-Z: 0 is the far plane; scene draws compare Greater. */
            gl.ClearDepth(0.0);
            clear_mask |= GL_DEPTH_BUFFER_BIT;
        }
        if (clear_mask)
            gl.Clear(clear_mask);
    }
    gl.Disable(GL_BLEND);
    gl.BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    gl.Enable(GL_DEPTH_TEST);
    gl.DepthMask(GL_TRUE);
    gl.PolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    gl.UseProgram(ctx->program);
    gl.BindVertexArray(ctx->vao);
}

/// @brief Backend `draw_skybox` op — type-cast wrapper over `gl_draw_skybox_impl`.
/// @param ctx_ptr Borrowed OpenGL backend context with an active frame.
/// @param cubemap_ptr Borrowed runtime cubemap to resolve and draw.
static void gl_draw_skybox(void *ctx_ptr, const void *cubemap_ptr) {
    gl_draw_skybox_impl((gl_context_t *)ctx_ptr, (const rt_cubemap3d *)cubemap_ptr);
}

/// @brief Backend `submit_draw` — emit a single mesh draw call.
///
/// Pipeline: backface cull / wireframe / poly-mode setup → mesh
/// VBO/IBO acquisition (cached or stream) → vertex attribute layout
/// → uniform upload (matrices, material, lights, fog, anim) →
/// texture binding → draw config (blend / depth) → `glDrawElements`.
/// Mark the RTT color dirty if rendering to RTT (so the next sync
/// readback knows to copy).
/// @param ctx_ptr Borrowed OpenGL backend context with an active draw target.
/// @param win Window handle retained for backend-interface compatibility; unused here.
/// @param cmd Borrowed mesh, material, animation, and per-draw state.
/// @param lights Borrowed light array; may be NULL when @p light_count is zero.
/// @param light_count Number of entries available through @p lights.
/// @param ambient Borrowed three-component ambient color; NULL is sanitized to black.
/// @param wireframe Nonzero to draw triangle edges instead of filled triangles.
/// @param backface_cull Nonzero to cull back-facing triangles.
static void gl_submit_draw(void *ctx_ptr,
                           vgfx_window_t win,
                           const vgfx3d_draw_cmd_t *cmd,
                           const vgfx3d_light_params_t *lights,
                           int32_t light_count,
                           const float *ambient,
                           int8_t wireframe,
                           int8_t backface_cull) {
    (void)win;
    gl_context_t *ctx = (gl_context_t *)ctx_ptr;
    GLuint mesh_vbo, mesh_ibo;
    GLint saved_uniform_locations[GL_MAIN_UNIFORM_SNAPSHOT_CAPACITY];
    GLuint saved_program = 0;
    int specialized_program;
    int mesh_compact = 0;
    uint32_t index_count;
    vgfx3d_draw_cmd_t safe_cmd;
    vgfx3d_light_params_t safe_lights[VGFX3D_MAX_LIGHTS];
    float safe_ambient[3];
    if (!ctx || !cmd || !cmd->vertices || !cmd->indices || cmd->vertex_count == 0 ||
        cmd->index_count == 0) {
        if (ctx)
            ctx->stats.dropped_draws++;
        return;
    }
    vgfx3d_sanitize_draw_command(cmd, &safe_cmd);
    cmd = &safe_cmd;
    light_count = vgfx3d_sanitize_light_array(lights, light_count, safe_lights, VGFX3D_MAX_LIGHTS);
    for (int32_t light_index = 0; light_index < light_count; light_index++)
        vgfx3d_sanitize_light_shadow_span(&safe_lights[light_index], ctx->shadow_count);
    lights = light_count > 0 ? safe_lights : NULL;
    vgfx3d_sanitize_ambient_rgb(ambient, safe_ambient);
    ambient = safe_ambient;
    index_count = vgfx3d_draw_cmd_validated_index_count(cmd);
    if (index_count == 0) {
        ctx->stats.dropped_draws++;
        return;
    }
    if (!prepare_mesh_buffers(ctx, cmd, &mesh_vbo, &mesh_ibo, &mesh_compact)) {
        ctx->stats.dropped_draws++;
        return;
    }

    gl.FrontFace(GL_CCW);
    if (backface_cull)
        gl.Enable(GL_CULL_FACE);
    else
        gl.Disable(GL_CULL_FACE);
    gl.PolygonMode(GL_FRONT_AND_BACK, wireframe ? GL_LINE : GL_FILL);
    gl_configure_draw_output(ctx, cmd);
    gl_apply_depth_bias(cmd, 1);

    specialized_program =
        gl_begin_material_program(ctx, cmd, saved_uniform_locations, &saved_program);
    gl.UseProgram(ctx->program);
    /* Resolve budgeted textures before IBL uniforms inspect native residency. */
    if (bind_material_textures(ctx, cmd) && cmd->texture_required) {
        /* Budget-paused text/image quad: draw nothing rather than a white block. */
        gl_end_material_program(ctx, specialized_program, saved_uniform_locations, saved_program);
        return;
    }
    upload_main_uniforms(ctx, cmd, lights, light_count, ambient, 0);
    configure_mesh_attributes(ctx, mesh_vbo, mesh_ibo, mesh_compact);
    if (gl_debug_enabled()) {
        GLint draw_buffer = 0;
        GLint framebuffer = 0;
        GLint viewport[4] = {0, 0, 0, 0};
        GLint scissor = 0;
        GLint depth_test = 0;
        GLint cull_face = 0;
        GLint color_mask[4] = {0, 0, 0, 0};
        gl.GetIntegerv(GL_DRAW_BUFFER, &draw_buffer);
        gl.GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &framebuffer);
        gl.GetIntegerv(GL_VIEWPORT, viewport);
        gl.GetIntegerv(GL_SCISSOR_TEST, &scissor);
        gl.GetIntegerv(GL_DEPTH_TEST, &depth_test);
        gl.GetIntegerv(GL_CULL_FACE, &cull_face);
        gl.GetIntegerv(GL_COLOR_WRITEMASK, color_mask);
        fprintf(stderr,
                "[OpenGL] draw target=%d fbo=%d drawbuf=0x%04X viewport=%d,%d %dx%d "
                "scissor=%d depth=%d cull=%d mask=%d%d%d%d verts=%u indices=%u ortho=%d "
                "first=(%.3f,%.3f,%.3f) color=(%.3f,%.3f,%.3f,%.3f) unlit=%d depthoff=%d\n",
                (int)ctx->active_target_kind,
                (int)framebuffer,
                (unsigned)draw_buffer,
                (int)viewport[0],
                (int)viewport[1],
                (int)viewport[2],
                (int)viewport[3],
                (int)scissor,
                (int)depth_test,
                (int)cull_face,
                (int)color_mask[0],
                (int)color_mask[1],
                (int)color_mask[2],
                (int)color_mask[3],
                cmd->vertex_count,
                index_count,
                (int)ctx->cam_is_ortho,
                cmd->vertices[0].pos[0],
                cmd->vertices[0].pos[1],
                cmd->vertices[0].pos[2],
                cmd->vertices[0].color[0],
                cmd->vertices[0].color[1],
                cmd->vertices[0].color[2],
                cmd->vertices[0].color[3],
                (int)cmd->unlit,
                (int)cmd->disable_depth_test);
    }
    gl.DrawElements(GL_TRIANGLES, (GLsizei)index_count, GL_UNSIGNED_INT, NULL);
    gl_end_material_program(ctx, specialized_program, saved_uniform_locations, saved_program);
    ctx->stats.draw_calls++;
    if (gl_debug_enabled()) {
        GLenum err = gl.GetError();
        if (err != GL_NO_ERROR)
            fprintf(stderr, "[OpenGL] draw error 0x%04X\n", (unsigned)err);
    }
    GL_CHECK();
}

/// @brief Issue this frame's queued scene-depth probe reads into the probe PBO.
/// @details Reads the scene FBO's depth attachment at each probe pixel; results are
///   harvested at the next frame's begin so the readback never syncs the pipeline.
/// @param ctx Borrowed OpenGL context containing queued probe coordinates and source depth.
static void gl_issue_depth_probes(gl_context_t *ctx) {
    gl_framebuffer_state_t saved_state;
    GLuint source_framebuffer;
    int32_t source_width;
    int32_t source_height;
    int reads_ok;

    if (!ctx || ctx->depth_probe_request_count <= 0)
        return;
    if (ctx->active_target_kind == VGFX3D_OPENGL_TARGET_SCENE) {
        source_framebuffer = ctx->scene_fbo;
        source_width = ctx->scene_width;
        source_height = ctx->scene_height;
        if (!ctx->scene_depth_tex)
            source_width = source_height = 0;
    } else if (ctx->active_target_kind == VGFX3D_OPENGL_TARGET_RTT) {
        source_framebuffer = ctx->rtt_fbo;
        source_width = ctx->rtt_width;
        source_height = ctx->rtt_height;
        if (!ctx->rtt_depth_rbo)
            source_width = source_height = 0;
    } else {
        source_framebuffer = 0;
        source_width = ctx->width;
        source_height = ctx->height;
    }
    if ((source_framebuffer == 0 && ctx->active_target_kind != VGFX3D_OPENGL_TARGET_SWAPCHAIN) ||
        source_width <= 0 || source_height <= 0) {
        ctx->depth_probe_request_count = 0;
        ctx->depth_probe_pending_count = 0;
        ctx->depth_probe_result_count = 0;
        return;
    }
    if (!ctx->depth_probe_pbo) {
        gl.GenBuffers(1, &ctx->depth_probe_pbo);
        if (!ctx->depth_probe_pbo) {
            ctx->depth_probe_request_count = 0;
            return;
        }
        gl.BindBuffer(GL_PIXEL_PACK_BUFFER, ctx->depth_probe_pbo);
        gl.BufferData(GL_PIXEL_PACK_BUFFER,
                      (GLsizeiptr)(VGFX3D_DEPTH_PROBE_MAX * sizeof(float)),
                      NULL,
                      GL_STREAM_READ);
        if (!gl_upload_ok()) {
            gl.BindBuffer(GL_PIXEL_PACK_BUFFER, 0);
            gl.DeleteBuffers(1, &ctx->depth_probe_pbo);
            ctx->depth_probe_pbo = 0;
            ctx->depth_probe_request_count = 0;
            ctx->depth_probe_pending_count = 0;
            ctx->depth_probe_result_count = 0;
            return;
        }
    } else {
        gl.BindBuffer(GL_PIXEL_PACK_BUFFER, ctx->depth_probe_pbo);
    }
    gl_capture_framebuffer_state(&saved_state);
    (void)gl_drain_errors("pre-depth-probe");
    gl.PixelStorei(GL_PACK_ALIGNMENT, 4);
    gl.BindFramebuffer(GL_READ_FRAMEBUFFER, source_framebuffer);
    for (int32_t i = 0; i < ctx->depth_probe_request_count; i++) {
        float ndc_x = ctx->depth_probe_requests[i][0];
        float ndc_y = ctx->depth_probe_requests[i][1];
        /* GL window origin is bottom-left, matching NDC y-up: no flip. */
        GLint px = (GLint)((ndc_x * 0.5f + 0.5f) * (float)source_width);
        GLint py = (GLint)((ndc_y * 0.5f + 0.5f) * (float)source_height);
        if (px < 0)
            px = 0;
        if (py < 0)
            py = 0;
        if (px >= source_width)
            px = source_width - 1;
        if (py >= source_height)
            py = source_height - 1;
        gl.ReadPixels(
            px, py, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, (void *)((size_t)i * sizeof(float)));
    }
    reads_ok = gl_drain_errors("depth-probe");
    if (reads_ok) {
        if (ctx->depth_probe_fence)
            gl.DeleteSync(ctx->depth_probe_fence);
        ctx->depth_probe_fence = gl.FenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        reads_ok = ctx->depth_probe_fence != NULL && gl_drain_errors("depth-probe-fence");
    }
    gl_restore_framebuffer_state(&saved_state);
    gl.BindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    ctx->depth_probe_pending_count = reads_ok ? ctx->depth_probe_request_count : 0;
    ctx->depth_probe_pending_polls = 0;
    if (!reads_ok) {
        if (ctx->depth_probe_fence)
            gl.DeleteSync(ctx->depth_probe_fence);
        ctx->depth_probe_fence = NULL;
        ctx->depth_probe_result_count = 0;
    }
    ctx->depth_probe_request_count = 0;
}

/// @brief Backend `end_frame` op: issue probes and mark RTT color dirty for lazy synchronization.
/// @details The function never reads render-target pixels immediately; the runtime invokes the
///          target's synchronization callback only when CPU color data is requested.
/// @param ctx_ptr Borrowed OpenGL backend context whose active frame is finalized.
static void gl_end_frame(void *ctx_ptr) {
    gl_context_t *ctx = (gl_context_t *)ctx_ptr;
    if (!ctx)
        return;

    ctx->frame_active = 0;

    gl_issue_depth_probes(ctx);
    if (ctx->rtt_active && ctx->rtt_target && ctx->rtt_color_tex) {
        ctx->rtt_color_dirty = 1;
        ctx->rtt_target->color_dirty = 1;
        ctx->rtt_target->hdr_color_valid = 0;
    }
}

/// @brief Backend hook: toggle native GLX/EGL swap-interval vsync.
/// @param ctx_ptr Borrowed OpenGL backend context containing the native presentation binding.
/// @param enabled Nonzero to request synchronization with the display refresh.
static void gl_set_vsync(void *ctx_ptr, int8_t enabled) {
    gl_context_t *ctx = (gl_context_t *)ctx_ptr;
    if (!ctx)
        return;
    if (!gl_make_current(ctx))
        return;
    if (ctx->egl_binding)
        (void)vgfx3d_egl_wayland_set_swap_interval(ctx->egl_binding, enabled ? 1 : 0);
    else if (glx.SwapIntervalEXT)
        glx.SwapIntervalEXT(ctx->display, ctx->window, enabled ? 1 : 0);
}

/// @brief Backend hook: register one NDC scene-depth probe for this frame.
/// @param ctx_ptr Borrowed OpenGL backend context receiving the request.
/// @param ndc_x Horizontal normalized-device coordinate, clamped to `[-1, 1]`.
/// @param ndc_y Vertical normalized-device coordinate, clamped to `[-1, 1]`.
/// @return The nonnegative request slot, or -1 when the request cannot be queued.
static int32_t gl_queue_depth_probe(void *ctx_ptr, float ndc_x, float ndc_y) {
    gl_context_t *ctx = (gl_context_t *)ctx_ptr;
    int32_t slot;
    if (!ctx || ctx->depth_probe_pending_count > 0 ||
        ctx->depth_probe_request_count >= VGFX3D_DEPTH_PROBE_MAX || !isfinite(ndc_x) ||
        !isfinite(ndc_y))
        return -1;
    slot = ctx->depth_probe_request_count++;
    /* Keep the later float-to-GLint viewport conversion in range. A finite but
     * enormous caller value would otherwise invoke undefined behavior before
     * the issue path had a chance to clamp the integer coordinate. */
    ctx->depth_probe_requests[slot][0] = vgfx3d_clamp_float_param(ndc_x, -1.0f, 1.0f, 0.0f);
    ctx->depth_probe_requests[slot][1] = vgfx3d_clamp_float_param(ndc_y, -1.0f, 1.0f, 0.0f);
    return slot;
}

/// @brief Backend hook: read a probe result captured on the previous frame.
/// @param ctx_ptr Borrowed OpenGL backend context containing harvested probe results.
/// @param slot Nonnegative slot previously returned by gl_queue_depth_probe().
/// @return The canonical scene depth, or -1 when no value is available for @p slot.
static float gl_read_depth_probe(void *ctx_ptr, int32_t slot) {
    gl_context_t *ctx = (gl_context_t *)ctx_ptr;
    if (!ctx || slot < 0 || slot >= ctx->depth_probe_result_count)
        return -1.0f;
    return ctx->depth_probe_results[slot];
}

/// @brief Read the total bytes uploaded to GL textures so far this frame (diagnostics counter).
/// @param ctx_ptr Borrowed OpenGL backend context; may be NULL.
/// @return The frame's accumulated upload bytes, or zero for a NULL context.
static uint64_t gl_get_texture_upload_bytes(void *ctx_ptr) {
    gl_context_t *ctx = (gl_context_t *)ctx_ptr;
    return ctx ? ctx->texture_upload_bytes : 0;
}

/// @brief Backend `resize` op — handle window-size changes.
///
/// Updates dimensions and tears down the offscreen scene + postfx
/// readback targets so they get rebuilt at the new size on the next
/// `BeginFrame`. RTT targets aren't touched (they're sized by the
/// user-provided RT object, not the window).
/// @param ctx_ptr Borrowed OpenGL backend context whose window resources become stale.
/// @param w New logical output width in pixels.
/// @param h New logical output height in pixels.
static void gl_resize(void *ctx_ptr, int32_t w, int32_t h) {
    gl_context_t *ctx = (gl_context_t *)ctx_ptr;
    if (!ctx || w <= 0 || h <= 0)
        return;
    ctx->width = w;
    ctx->height = h;
    ctx->scene_composited_to_backbuffer = 0;
    if (ctx->egl_binding)
        vgfx3d_egl_wayland_resize(ctx->egl_binding, w, h);
    if (!gl_make_current(ctx))
        return;
    if (!ctx->rtt_active) {
        destroy_scene_targets(ctx);
        destroy_postfx_readback_target(ctx);
        destroy_postfx_scratch_target(ctx);
        destroy_present_target(ctx);
        destroy_bloom_targets(ctx);
        destroy_taa_targets(ctx);
        destroy_ssr_target(ctx);
    }
}

/// @brief Ensure the context has a reusable host-side RGBA readback buffer.
/// @details GPU readback paths are called by screenshots, overlays, and the X11 fallback
///          presenter. Keeping one scratch buffer on the context avoids allocating and freeing
///          a large temporary every frame. Geometric growth also avoids reallocating for every
///          small resolution increase while preserving the exact byte-count overflow checks in
///          each caller.
/// @param ctx OpenGL backend context that owns the scratch allocation.
/// @param bytes Minimum number of bytes required by the caller.
/// @return Naturally aligned raw storage for at least @p bytes, or NULL on allocation failure.
static void *gl_ensure_readback_scratch(gl_context_t *ctx, size_t bytes) {
    size_t capacity;
    uint8_t *next;

    if (!ctx || bytes == 0)
        return NULL;
    if (ctx->readback_scratch_rgba && ctx->readback_scratch_bytes >= bytes)
        return ctx->readback_scratch_rgba;
    capacity = ctx->readback_scratch_bytes;
    if (capacity < 4096u)
        capacity = 4096u;
    while (capacity < bytes) {
        const size_t increment = capacity / 2u + 1u;
        if (increment > SIZE_MAX - capacity) {
            capacity = bytes;
            break;
        }
        capacity += increment;
    }
    next = (uint8_t *)realloc(ctx->readback_scratch_rgba, capacity);
    if (!next)
        return NULL;
    ctx->readback_scratch_rgba = next;
    ctx->readback_scratch_bytes = capacity;
    return next;
}

/// @brief Backend `readback_rgba` op — copy current screen to a host RGBA buffer.
///
/// Two paths:
///   - Postfx readback: when scene is rendered offscreen with postfx
///     pending, runs the composite shader into a staging FBO and reads
///     from it. Gives the user the post-FX'd image even though it
///     hasn't been presented yet.
///   - Direct readback: pulls from the context's active default framebuffer buffer.
/// Always flips Y because GL has bottom-left origin; ours is top-left.
/// @param ctx_ptr Borrowed OpenGL backend context providing the current final image.
/// @param[out] dst_rgba Caller-owned RGBA8 destination buffer.
/// @param w Requested destination width in pixels.
/// @param h Requested destination height in pixels.
/// @param stride Destination row pitch in bytes.
/// @return 1 after successful readback and vertical flip, otherwise 0.
static int gl_readback_rgba(
    void *ctx_ptr, uint8_t *dst_rgba, int32_t w, int32_t h, int32_t stride) {
    gl_context_t *ctx = (gl_context_t *)ctx_ptr;
    int use_postfx_readback;
    int32_t source_w, source_h;
    int32_t copy_w, copy_h;
    GLuint readback_framebuffer = 0;
    GLenum readback_buffer = ctx ? ctx->default_draw_buffer : GL_BACK;
    uint8_t *tmp;
    size_t tmp_bytes;
    size_t dst_bytes;
    gl_framebuffer_state_t saved_state;

    if (!ctx || !dst_rgba || !vgfx3d_opengl_validate_rgba8_destination(w, h, stride, &dst_bytes))
        return 0;

    if (!gl_make_current(ctx))
        return 0;
    use_postfx_readback = 0;
    if (gl_window_scene_route_enabled(ctx) && ctx->scene_postfx_pending &&
        !ctx->scene_composited_to_backbuffer && ctx->scene_fbo && ctx->scene_width > 0 &&
        ctx->scene_height > 0 && ensure_present_target(ctx, ctx->width, ctx->height) == 0) {
        int composite_ok = 0;
        if (ctx->gpu_postfx_enabled && ctx->gpu_postfx_chain_valid &&
            vgfx3d_postfx_chain_is_usable(&ctx->gpu_postfx_chain)) {
            composite_ok = gl_apply_postfx_chain(ctx,
                                                 ctx->scene_color_tex,
                                                 ctx->scene_width,
                                                 ctx->scene_height,
                                                 &ctx->gpu_postfx_chain,
                                                 ctx->present_fbo,
                                                 GL_COLOR_ATTACHMENT0,
                                                 ctx->width,
                                                 ctx->height,
                                                 0,
                                                 NULL,
                                                 NULL);
        }
        if (!composite_ok) {
            composite_ok = gl_draw_texture_to_target(ctx,
                                                     ctx->scene_color_tex,
                                                     ctx->present_fbo,
                                                     GL_COLOR_ATTACHMENT0,
                                                     ctx->scene_width,
                                                     ctx->scene_height,
                                                     ctx->width,
                                                     ctx->height,
                                                     NULL);
        }
        if (composite_ok) {
            readback_framebuffer = ctx->present_fbo;
            readback_buffer = GL_COLOR_ATTACHMENT0;
            source_w = ctx->present_width;
            source_h = ctx->present_height;
            use_postfx_readback = 1;
        } else {
            readback_framebuffer = ctx->scene_fbo;
            readback_buffer = GL_COLOR_ATTACHMENT0;
            source_w = ctx->scene_width;
            source_h = ctx->scene_height;
            use_postfx_readback = 1;
        }
    } else if (ctx->present_capture_valid && ctx->present_fbo && ctx->present_tex) {
        /* A frame was presented since the last render, so the back buffer no
         * longer holds it; read the copy taken just before the swap. */
        readback_framebuffer = ctx->present_fbo;
        readback_buffer = GL_COLOR_ATTACHMENT0;
        source_w = ctx->present_width;
        source_h = ctx->present_height;
        use_postfx_readback = 1;
    } else if (!ctx->default_framebuffer_writable && ctx->present_fbo && ctx->present_tex) {
        readback_framebuffer = ctx->present_fbo;
        readback_buffer = GL_COLOR_ATTACHMENT0;
        source_w = ctx->present_width;
        source_h = ctx->present_height;
        use_postfx_readback = 1;
    } else if (!ctx->default_framebuffer_writable && ctx->scene_fbo) {
        readback_framebuffer = ctx->scene_fbo;
        readback_buffer = GL_COLOR_ATTACHMENT0;
        source_w = ctx->scene_width;
        source_h = ctx->scene_height;
        use_postfx_readback = 1;
    } else {
        source_w = ctx->width;
        source_h = ctx->height;
    }

    copy_w = source_w < w ? source_w : w;
    copy_h = source_h < h ? source_h : h;
    if (copy_w <= 0 || copy_h <= 0)
        return 0;
    if ((size_t)copy_w > SIZE_MAX / (size_t)copy_h ||
        (size_t)copy_w * (size_t)copy_h > SIZE_MAX / 4u) {
        return 0;
    }
    tmp_bytes = (size_t)copy_w * (size_t)copy_h * 4u;
    tmp = gl_ensure_readback_scratch(ctx, tmp_bytes);
    if (!tmp)
        return 0;

    memset(dst_rgba, 0, dst_bytes);
    gl_capture_framebuffer_state(&saved_state);
    (void)gl_drain_errors("pre-readback");
    gl.PixelStorei(GL_PACK_ALIGNMENT, 1);
    gl.BindFramebuffer(GL_FRAMEBUFFER, use_postfx_readback ? readback_framebuffer : 0);
    gl.ReadBuffer(use_postfx_readback ? readback_buffer : ctx->default_draw_buffer);
    gl.ReadPixels(0, 0, copy_w, copy_h, GL_RGBA, GL_UNSIGNED_BYTE, tmp);
    if (!gl_drain_errors("readback")) {
        gl_restore_framebuffer_state(&saved_state);
        gl_restore_main_draw_state(ctx);
        return 0;
    }
    vgfx3d_flip_rgba_rows(tmp, copy_w, copy_h);
    for (int32_t y = 0; y < copy_h; y++)
        memcpy(dst_rgba + (size_t)y * (size_t)stride,
               tmp + (size_t)y * (size_t)copy_w * 4u,
               (size_t)copy_w * 4u);
    gl_restore_framebuffer_state(&saved_state);
    gl_restore_main_draw_state(ctx);
    return 1;
}

/// @brief Present an offscreen OpenGL final target through the ZannaGFX window buffer.
/// @details This fallback is used only when the GLX default framebuffer probe shows that the
///          window drawable does not accept RGB writes. Rendering and post-FX remain on OpenGL;
///          the resolved final FBO is copied to the existing RGBA ZannaGFX framebuffer and
///          presented through the platform XImage path.
/// @param ctx Borrowed OpenGL context providing the final FBO and ZannaGFX window.
/// @return 1 when the image is copied and presented, otherwise 0.
static int gl_present_offscreen_vgfx(gl_context_t *ctx) {
    vgfx_framebuffer_t fb;
    GLuint framebuffer;
    int32_t source_w;
    int32_t source_h;
    int32_t copy_w;
    int32_t copy_h;
    uint8_t *tmp;
    size_t tmp_bytes;
    gl_framebuffer_state_t saved_state;

    if (!ctx || !ctx->vgfx_win)
        return 0;
    if (ctx->present_fbo && ctx->present_tex && ctx->present_width > 0 && ctx->present_height > 0 &&
        ctx->scene_composited_to_backbuffer) {
        framebuffer = ctx->present_fbo;
        source_w = ctx->present_width;
        source_h = ctx->present_height;
    } else if (!ctx->default_framebuffer_writable && ctx->scene_fbo) {
        framebuffer = ctx->scene_fbo;
        source_w = ctx->scene_width;
        source_h = ctx->scene_height;
    } else if (ctx->scene_fbo) {
        framebuffer = ctx->scene_fbo;
        source_w = ctx->scene_width;
        source_h = ctx->scene_height;
    } else {
        return 0;
    }
    if (!vgfx_get_framebuffer(ctx->vgfx_win, &fb) || !fb.pixels || fb.width <= 0 ||
        fb.height <= 0 || fb.stride <= 0 || (size_t)fb.stride < (size_t)fb.width * 4u ||
        source_w <= 0 || source_h <= 0) {
        return 0;
    }
    copy_w = source_w < fb.width ? source_w : fb.width;
    copy_h = source_h < fb.height ? source_h : fb.height;
    if (copy_w <= 0 || copy_h <= 0 || (size_t)copy_w > SIZE_MAX / (size_t)copy_h ||
        (size_t)copy_w * (size_t)copy_h > SIZE_MAX / 4u) {
        return 0;
    }
    tmp_bytes = (size_t)copy_w * (size_t)copy_h * 4u;
    tmp = gl_ensure_readback_scratch(ctx, tmp_bytes);
    if (!tmp)
        return 0;

    gl_capture_framebuffer_state(&saved_state);
    gl.PixelStorei(GL_PACK_ALIGNMENT, 1);
    gl.BindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    gl.ReadBuffer(GL_COLOR_ATTACHMENT0);
    gl.ReadPixels(0, 0, copy_w, copy_h, GL_RGBA, GL_UNSIGNED_BYTE, tmp);
    gl.PixelStorei(GL_PACK_ALIGNMENT, 4);
    vgfx3d_flip_rgba_rows(tmp, copy_w, copy_h);
    for (int32_t y = 0; y < copy_h; y++)
        memcpy((uint8_t *)fb.pixels + (size_t)y * (size_t)fb.stride,
               tmp + (size_t)y * (size_t)copy_w * 4u,
               (size_t)copy_w * 4u);
    gl_restore_framebuffer_state(&saved_state);
    gl_restore_main_draw_state(ctx);

    vgfx_set_gpu_present(ctx->vgfx_win, 0);
    (void)vgfx_update(ctx->vgfx_win);
    vgfx_set_gpu_present(ctx->vgfx_win, 1);
    return 1;
}

/// @brief Internal present — composite scene to backbuffer (if needed) then SwapBuffers.
///
/// Skips the composite when scene was never rendered offscreen (no
/// postfx). After present, clears the per-frame state flags so the
/// next frame starts clean.
/// @param ctx Borrowed OpenGL context whose window frame is presented.
/// @param chain Borrowed post-processing chain to composite, or NULL for a plain resolve.
/// @brief Copy the composited back buffer into the present target before a swap.
/// @details Serves the `set_capture_after_present` contract on this backend: the
///          swap leaves the back buffer undefined, so a readback that follows a
///          present must come from a copy taken while the frame was still there.
///          A failure leaves `present_capture_valid` clear, and the readback path
///          falls back to its previous default-framebuffer source.
/// @param ctx Borrowed context whose current back buffer should be retained.
static void gl_capture_presented_frame(gl_context_t *ctx) {
    gl_framebuffer_state_t saved;

    if (!ctx || !gl.BlitFramebuffer || ctx->width <= 0 || ctx->height <= 0)
        return;
    ctx->present_capture_valid = 0;
    if (ensure_present_target(ctx, ctx->width, ctx->height) != 0)
        return;

    gl_capture_framebuffer_state(&saved);
    (void)gl_drain_errors("pre-present-capture");
    gl.BindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    gl.ReadBuffer(ctx->default_draw_buffer);
    gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, ctx->present_fbo);
    gl.DrawBuffer(GL_COLOR_ATTACHMENT0);
    gl.BlitFramebuffer(0,
                       0,
                       ctx->width,
                       ctx->height,
                       0,
                       0,
                       ctx->present_width,
                       ctx->present_height,
                       GL_COLOR_BUFFER_BIT,
                       GL_NEAREST);
    ctx->present_capture_valid = gl_drain_errors("present-capture") ? 1 : 0;
    gl_restore_framebuffer_state(&saved);
}

static void gl_present_impl(gl_context_t *ctx, const vgfx3d_postfx_chain_t *chain) {
    int use_postfx;

    if (!ctx || ctx->rtt_active)
        return;
    if (!gl_make_current(ctx))
        return;
    use_postfx = (vgfx3d_postfx_chain_is_usable(chain) && ctx->gpu_postfx_enabled &&
                  ctx->scene_postfx_pending)
                     ? 1
                     : 0;
    if (gl_window_scene_route_enabled(ctx) && ctx->scene_postfx_pending &&
        !ctx->scene_composited_to_backbuffer) {
        if (draw_scene_texture(ctx, use_postfx ? chain : NULL)) {
            ctx->scene_composited_to_backbuffer = 1;
        } else if (ctx->default_framebuffer_writable) {
            /* The writability probe passed but the fullscreen resolve to the default
             * drawable did not land — the documented GLX failure mode. Demote to the
             * ZannaGFX route permanently and retry; draw_scene_texture now targets the
             * backend-owned present FBO, so this frame still composites correctly. */
            gl_demote_to_offscreen_present(ctx);
            if (draw_scene_texture(ctx, use_postfx ? chain : NULL))
                ctx->scene_composited_to_backbuffer = 1;
        }
    }
    if (gl_debug_enabled()) {
        GLint draw_buffer = 0;
        GLint framebuffer = 0;
        gl.GetIntegerv(GL_DRAW_BUFFER, &draw_buffer);
        gl.GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &framebuffer);
        fprintf(stderr,
                "[OpenGL] present fbo=%d drawbuf=0x%04X postfx_enabled=%d pending=%d "
                "composited=%d use_postfx=%d\n",
                (int)framebuffer,
                (unsigned)draw_buffer,
                (int)ctx->gpu_postfx_enabled,
                (int)ctx->scene_postfx_pending,
                (int)ctx->scene_composited_to_backbuffer,
                use_postfx);
    }
    /* Close the frame's timestamp pair before the present path runs, so the measured
     * span covers this frame's GPU work and not the next frame's submissions. */
    if (ctx->timer_queries_ready && ctx->timer_slot_active >= 0) {
        gl.QueryCounter(ctx->timer_query[ctx->timer_slot_active][1], GL_TIMESTAMP);
        ctx->timer_query_pending[ctx->timer_slot_active] = 1;
        ctx->timer_query_head = (ctx->timer_slot_active + 1) % VGFX3D_GL_TIMER_FRAMES;
        ctx->timer_slot_active = -1;
    }
    if (!ctx->default_framebuffer_writable) {
        (void)gl_present_offscreen_vgfx(ctx);
        ctx->stats.offscreen_presents++;
        ctx->stats.present_path = 2;
        ctx->scene_postfx_pending = 0;
        ctx->scene_composited_to_backbuffer = 0;
        return;
    }
    /* Swapping hands the back buffer to the display and leaves its contents
     * undefined, so a readback taken after presenting has nothing to read. Keep
     * a copy first when the caller has declared that presented frames must stay
     * readable; retaining unconditionally would charge every rendered frame a
     * full-screen blit and state round-trip for a capture that usually never
     * comes. */
    if (ctx->capture_after_present)
        gl_capture_presented_frame(ctx);
    gl_swap_native_buffers(ctx);
    ctx->stats.direct_presents++;
    ctx->stats.present_path = 1;
    ctx->scene_postfx_pending = 0;
    ctx->scene_composited_to_backbuffer = 0;
}

/// @brief Backend `present` (no postfx) — direct SwapBuffers via `gl_present_impl`.
/// @param ctx_ptr Borrowed OpenGL backend context to present.
static void gl_present(void *ctx_ptr) {
    gl_present_impl((gl_context_t *)ctx_ptr, NULL);
}

/// @brief Backend `get_frame_gpu_time_us` op — latest retired GPU frame duration (GAP-9).
/// @details Polls the timestamp ring without blocking, so the value trails by up to
///   `VGFX3D_GL_TIMER_FRAMES` frames and reads 0 until the first pair retires or when
///   ARB_timer_query is unavailable.
/// @param ctx_ptr Borrowed OpenGL backend context.
/// @return Duration in microseconds, or zero when no result is available yet.
static uint64_t gl_get_frame_gpu_time_us(void *ctx_ptr) {
    gl_context_t *ctx = (gl_context_t *)ctx_ptr;
    if (!ctx || !ctx->timer_queries_ready)
        return 0;
    if (!gl_make_current(ctx))
        return ctx->frame_gpu_time_us;
    gl_harvest_timer_queries(ctx);
    return ctx->frame_gpu_time_us;
}

/// @brief Backend `set_capture_after_present` op — keep presented frames readable.
/// @details This backend reads back from the window's back buffer, which a swap
///          leaves undefined, so it needs the hook for the same reason the Metal
///          backend does. Disabling drops any retained frame so a later readback
///          resolves against live targets instead of a stale copy.
/// @param ctx_ptr Borrowed OpenGL backend context.
/// @param enabled Nonzero to retain each presented frame for readback.
static void gl_set_capture_after_present(void *ctx_ptr, int8_t enabled) {
    gl_context_t *ctx = (gl_context_t *)ctx_ptr;
    if (!ctx)
        return;
    ctx->capture_after_present = enabled ? 1 : 0;
    if (!ctx->capture_after_present)
        ctx->present_capture_valid = 0;
}

/// @brief Backend `present_postfx` hook — composite the scene through the supplied chain and swap.
/// @details Canvas3D calls this legacy single-step hook when the active frame does not need
///          final-overlay replay after post-processing. The implementation delegates to the
///          shared presenter so a disabled or empty chain still resolves the offscreen scene to
///          the swapchain before presentation.
/// @param ctx_ptr Borrowed OpenGL backend context to composite and present.
/// @param postfx Borrowed post-processing chain, or an empty chain for a plain resolve.
static void gl_present_postfx(void *ctx_ptr, const vgfx3d_postfx_chain_t *postfx) {
    gl_present_impl((gl_context_t *)ctx_ptr, postfx);
}

/// @brief Backend split post-FX hook — composite pending scene color without swapping.
/// @details This is used when Canvas3D must replay final overlays after post-FX or capture a
///          finalized screenshot without presenting the drawable. It writes the pending scene
///          target into the backbuffer and marks it composited; a later `present` call performs
///          the actual buffer swap.
/// @param ctx_ptr Borrowed OpenGL backend context containing the pending scene.
/// @param postfx Borrowed post-processing chain, or an empty chain for a plain resolve.
static void gl_apply_postfx(void *ctx_ptr, const vgfx3d_postfx_chain_t *postfx) {
    gl_context_t *ctx = (gl_context_t *)ctx_ptr;
    int use_postfx;
    if (!ctx || ctx->rtt_active)
        return;
    if (!gl_make_current(ctx))
        return;
    use_postfx = (vgfx3d_postfx_chain_is_usable(postfx) && ctx->gpu_postfx_enabled &&
                  ctx->scene_postfx_pending)
                     ? 1
                     : 0;
    if (gl_window_scene_route_enabled(ctx) && ctx->scene_postfx_pending &&
        !ctx->scene_composited_to_backbuffer) {
        int drawn = draw_scene_texture(ctx, use_postfx ? postfx : NULL);
        if (drawn)
            ctx->scene_composited_to_backbuffer = 1;
    }
}

/// @brief Backend `set_gpu_postfx_enabled` hook — route future frames through scene targets.
/// @details When enabled, `begin_frame` chooses the offscreen scene FBO so post-processing can
///          sample the rendered color/depth/motion buffers. Disabling post-FX clears latched
///          chain state and any pending scene-composite flags so the next frame returns to direct
///          swapchain rendering.
/// @param ctx_ptr Borrowed OpenGL backend context whose scene route is updated.
/// @param enabled Nonzero to enable the GPU post-processing route.
static void gl_set_gpu_postfx_enabled(void *ctx_ptr, int8_t enabled) {
    gl_context_t *ctx = (gl_context_t *)ctx_ptr;
    int8_t next_enabled = enabled ? 1 : 0;
    if (!ctx)
        return;
    if (ctx->gpu_postfx_enabled != next_enabled) {
        ctx->scene_postfx_pending = 0;
        ctx->scene_composited_to_backbuffer = 0;
    }
    ctx->gpu_postfx_enabled = next_enabled;
    if (!ctx->gpu_postfx_enabled) {
        vgfx3d_postfx_chain_reset(&ctx->gpu_postfx_chain);
        ctx->gpu_postfx_chain_valid = 0;
    }
}

/// @brief Apply a reduced window-scene render scale to the OpenGL backend.
/// @details Scale changes are accepted only outside an active or pending frame. The function
///          rebuilds scene color/motion/depth at the shared scaled extent and creates a distinct
///          logical-output-sized presentation target used by screenshots and the X11 fallback.
///          The existing full-screen post-FX shader performs the explicit upscale even when no
///          user effects are active. On allocation failure, the prior scale is restored and its
///          window route is reconstructed. Explicit RTT dimensions are unaffected.
/// @param ctx_ptr Borrowed `gl_context_t` backend handle.
/// @param scale Finite requested scale in `[0.25, 1]`; values at least `0.999` normalize to 1.
/// @return 1 when the requested/native route is ready, or 0 for invalid input, an active frame,
///         missing composite shader support, context activation failure, or target allocation
///         failure.
static int8_t gl_set_render_scale(void *ctx_ptr, float scale) {
    gl_context_t *ctx = (gl_context_t *)ctx_ptr;
    float prior_scale;
    float next_scale;
    int32_t scene_width;
    int32_t scene_height;
    int route_ready = 1;

    if (!ctx || !vgfx3d_compute_scaled_scene_extent(
                    ctx->width, ctx->height, scale, &scene_width, &scene_height)) {
        return 0;
    }
    next_scale = scale >= 0.999f ? 1.0f : scale;
    if (next_scale < 1.0f && !ctx->postfx_program)
        return 0;
    if (ctx->frame_active || ctx->scene_postfx_pending)
        return 0;
    if (!gl_make_current(ctx))
        return 0;

    prior_scale =
        (isfinite(ctx->render_scale) && ctx->render_scale >= 0.25f && ctx->render_scale <= 1.0f)
            ? ctx->render_scale
            : 1.0f;
    if (fabsf(prior_scale - next_scale) <= 1.0e-6f)
        return 1;

    destroy_scene_targets(ctx);
    destroy_postfx_readback_target(ctx);
    destroy_postfx_scratch_target(ctx);
    destroy_present_target(ctx);
    destroy_bloom_targets(ctx);
    destroy_taa_targets(ctx);
    destroy_ssr_target(ctx);
    ctx->render_scale = next_scale;
    if (gl_window_scene_route_enabled(ctx)) {
        if (!vgfx3d_compute_scaled_scene_extent(
                ctx->width, ctx->height, next_scale, &scene_width, &scene_height) ||
            ensure_scene_targets(ctx, scene_width, scene_height) != 0 ||
            ensure_present_target(ctx, ctx->width, ctx->height) != 0) {
            route_ready = 0;
        }
    }
    if (!route_ready) {
        destroy_scene_targets(ctx);
        destroy_present_target(ctx);
        ctx->render_scale = prior_scale;
        if (gl_window_scene_route_enabled(ctx) &&
            vgfx3d_compute_scaled_scene_extent(
                ctx->width, ctx->height, prior_scale, &scene_width, &scene_height)) {
            (void)ensure_scene_targets(ctx, scene_width, scene_height);
            (void)ensure_present_target(ctx, ctx->width, ctx->height);
        }
    }
    ctx->scene_postfx_pending = 0;
    ctx->scene_composited_to_backbuffer = 0;
    ctx->active_target_kind =
        vgfx3d_opengl_choose_target_kind(ctx->rtt_active, gl_window_scene_route_enabled(ctx));
    bind_main_framebuffer(ctx);
    return route_ready ? 1 : 0;
}

/// @brief Backend `set_gpu_postfx_snapshot` hook — latch a stable copy of the frame chain.
/// @details The Canvas3D-side chain is mutable after frame setup, so the backend stores its own
///          copy for present-time composition and readback. Snapshot refresh can also occur while
///          replaying final overlays after `apply_postfx`; it must preserve an already-composited
///          backbuffer so overlay replay is not overwritten by a second scene composite. Copy
///          failure resets the cached chain instead of leaving partial effect state visible to
///          later frames.
/// @param ctx_ptr Borrowed OpenGL backend context that owns the resulting chain copy.
/// @param postfx Borrowed chain to copy, or NULL to clear the installed snapshot.
static void gl_set_gpu_postfx_snapshot(void *ctx_ptr, const vgfx3d_postfx_chain_t *postfx) {
    gl_context_t *ctx = (gl_context_t *)ctx_ptr;
    if (!ctx)
        return;
    if (!postfx) {
        vgfx3d_postfx_chain_reset(&ctx->gpu_postfx_chain);
        ctx->gpu_postfx_chain_valid = 0;
        ctx->taa_history_valid = 0;
        return;
    }
    if (!vgfx3d_postfx_chain_copy(&ctx->gpu_postfx_chain, postfx)) {
        vgfx3d_postfx_chain_reset(&ctx->gpu_postfx_chain);
        ctx->gpu_postfx_chain_valid = 0;
        ctx->taa_history_valid = 0;
        return;
    }
    ctx->gpu_postfx_chain_valid = 1;
    /* Plan 05: dropping TAA from the chain invalidates temporal history so a later
     * re-enable starts from a fresh accumulation instead of stale frames. */
    if (!gl_chain_has_effect(postfx, (int32_t)VGFX3D_POSTFX_EFFECT_TAA))
        ctx->taa_history_valid = 0;
}

/// @brief Backend `set_render_target` op — bind / unbind RTT.
///
/// NULL `rt` tears down RTT and reverts to the swapchain. Non-NULL
/// allocates / re-binds RTT targets sized for `rt`. Clears any
/// in-flight scene postfx state since RTT bypasses that path.
/// @param ctx_ptr Borrowed OpenGL backend context whose target routing is changed.
/// @param rt Borrowed render target to activate, or NULL to restore window rendering.
static void gl_set_render_target(void *ctx_ptr, vgfx3d_rendertarget_t *rt) {
    gl_context_t *ctx = (gl_context_t *)ctx_ptr;
    if (!ctx)
        return;
    if (!gl_make_current(ctx))
        return;
    ctx->scene_postfx_pending = 0;
    ctx->scene_composited_to_backbuffer = 0;
    if (!rt) {
        /* A void target-switch API cannot report readback failure. Keep the dirty target and its
         * synchronization hook live so a later retry can still recover the only authoritative
         * GPU copy instead of deleting it during teardown. */
        if (ctx->rtt_target && (ctx->rtt_color_dirty || ctx->rtt_target->color_dirty) &&
            !gl_sync_render_target_color(ctx, ctx->rtt_target)) {
            return;
        }
        destroy_rtt_targets(ctx);
        gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
        return;
    }
    if (ensure_rtt_targets(ctx, rt) != 0) {
        /* ensure_rtt_targets is transactional. Preserve a prior active RTT after either
         * replacement allocation or dirty synchronization fails; only select the swapchain when
         * no complete RTT existed before this request. */
        if (!ctx->rtt_fbo || !ctx->rtt_color_tex || !ctx->rtt_depth_rbo || !ctx->rtt_target) {
            ctx->rtt_active = 0;
            ctx->active_target_kind = VGFX3D_OPENGL_TARGET_SWAPCHAIN;
            gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
        }
    }
}

/// @brief Backend `resolve_opaque_targets` op — snapshot the scene depth at the
///   opaque->transparent seam. The live depth attachment cannot be sampled by
///   the very pass writing it, so the depth planes are blitted into a dedicated
///   snapshot texture that blend draws (soft particles) read via
///   `uOpaqueDepthTex`. The currently bound framebuffer is restored afterwards.
/// @param ctx_ptr Borrowed OpenGL backend context whose active depth attachment is resolved.
static void gl_resolve_opaque_targets(void *ctx_ptr) {
    gl_context_t *ctx = (gl_context_t *)ctx_ptr;
    GLint prev_fbo = 0;
    int32_t w;
    int32_t h;

    if (!ctx || !gl.BlitFramebuffer)
        return;
    ctx->opaque_depth_valid = 0;
    w = ctx->rtt_active ? ctx->rtt_width : ctx->width;
    h = ctx->rtt_active ? ctx->rtt_height : ctx->height;
    if (w <= 0 || h <= 0)
        return;
    gl.GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev_fbo);
    if (!ctx->opaque_depth_tex)
        gl.GenTextures(1, &ctx->opaque_depth_tex);
    if (!ctx->opaque_depth_fbo)
        gl.GenFramebuffers(1, &ctx->opaque_depth_fbo);
    if (!ctx->opaque_depth_tex || !ctx->opaque_depth_fbo)
        return;
    if (ctx->opaque_depth_w != w || ctx->opaque_depth_h != h) {
        gl.BindTexture(GL_TEXTURE_2D, ctx->opaque_depth_tex);
        gl.TexImage2D(
            GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT32F, w, h, 0, GL_DEPTH_COMPONENT, GL_FLOAT, NULL);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        gl.BindTexture(GL_TEXTURE_2D, 0);
        gl.BindFramebuffer(GL_FRAMEBUFFER, ctx->opaque_depth_fbo);
        gl.FramebufferTexture2D(
            GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, ctx->opaque_depth_tex, 0);
        gl.DrawBuffer(GL_NONE);
        gl.ReadBuffer(GL_NONE);
        ctx->opaque_depth_w = w;
        ctx->opaque_depth_h = h;
    }
    gl.BindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)prev_fbo);
    gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, ctx->opaque_depth_fbo);
    gl.BlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
    gl.BindFramebuffer(GL_FRAMEBUFFER, (GLuint)prev_fbo);
    ctx->opaque_depth_valid = gl_upload_ok() ? 1 : 0;
}

/// @brief Abort an active shadow pass and restore color writes plus the main framebuffer.
/// @param ctx Borrowed OpenGL context whose shadow state is left.
static void gl_leave_shadow_pass(gl_context_t *ctx) {
    if (!ctx || ctx->shadow_pass_slot < 0)
        return;
    ctx->shadow_pass_slot = -1;
    ctx->shadow_pass_failed = 0;
    gl_recompute_shadow_count(ctx);
    gl.Disable(GL_POLYGON_OFFSET_FILL);
    gl.ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    bind_main_framebuffer(ctx);
    gl.UseProgram(ctx->program);
}

/// @brief Backend `shadow_begin` — switch the pipeline to shadow-pass mode.
///
/// Allocates the shadow FBO if needed, captures the light-space VP,
/// binds the depth-only FBO + viewport, clears depth, switches to
/// the shadow vertex shader (no PS).
/// @param ctx_ptr Borrowed OpenGL backend context that owns shadow resources.
/// @param slot Shadow-light slot to initialize.
/// @param depth_buf Ignored CPU buffer retained for backend-interface compatibility.
/// @param w Requested shadow-map width in pixels.
/// @param h Requested shadow-map height in pixels.
/// @param light_vp Borrowed row-major light view-projection matrix.
static void gl_shadow_begin(
    void *ctx_ptr, int32_t slot, float *depth_buf, int32_t w, int32_t h, const float *light_vp) {
    (void)depth_buf;
    gl_context_t *ctx = (gl_context_t *)ctx_ptr;
    if (!ctx)
        return;
    gl_leave_shadow_pass(ctx);
    if (slot < 0 || slot >= VGFX3D_MAX_SHADOW_LIGHTS)
        return;
    ctx->shadow_complete[slot] = 0;
    ctx->shadow_pass_failed = 0;
    gl_recompute_shadow_count(ctx);
    if (!vgfx3d_shadow_matrix_is_usable(light_vp) || w <= 0 || h <= 0)
        return;
    if (ensure_shadow_targets(ctx, slot, w, h) != 0) {
        ctx->shadow_complete[slot] = 0;
        gl_recompute_shadow_count(ctx);
        return;
    }
    ctx->shadow_pass_slot = slot;
    vgfx3d_copy_mat4_finite_or_identity(ctx->shadow_vp[slot], light_vp);
    /* ADR 0246: every slot is a layer of one array texture, so the main pass's
     * sampler binding on GL_TU_SHADOW_ARRAY covers the very layer this pass is about
     * to render into — a texture feedback loop, which is undefined and which this
     * driver reports as GL_INVALID_OPERATION. With per-slot 2D textures the bound
     * texture was never the render target, so the hazard did not exist. Drop the
     * binding for the duration of the pass; the main pass rebinds it per draw. */
    gl.ActiveTexture(GL_TEXTURE0 + (GLenum)GL_TU_SHADOW_ARRAY);
    gl.BindTexture(GL_TEXTURE_2D_ARRAY, 0);
    gl.ActiveTexture(GL_TEXTURE0);
    gl.BindFramebuffer(GL_FRAMEBUFFER, ctx->shadow_fbo[slot]);
    /* The shared array may exceed this slot's requested size (grow-only policy), so
     * render across the full layer to keep sample UVs spanning [0,1]. */
    gl.Viewport(0, 0, ctx->shadow_array_width, ctx->shadow_array_height);
    /* Shadow maps keep the standard depth convention: clear 1, compare Less. The
     * func is pinned here because scene draws leave Greater (reversed-Z) behind. */
    gl.ClearDepth(1.0);
    gl.Clear(GL_DEPTH_BUFFER_BIT);
    gl.Enable(GL_DEPTH_TEST);
    gl.DepthFunc(GL_LESS);
    gl.DepthMask(GL_TRUE);
    gl.ColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    gl.Enable(GL_CULL_FACE);
    gl.CullFace(GL_BACK);
    gl.FrontFace(GL_CCW);
    gl.Disable(GL_POLYGON_OFFSET_FILL);
    gl.PolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    gl.UseProgram(ctx->shadow_program);
    gl.BindVertexArray(ctx->vao);
}

/// @brief Backend `shadow_draw` — emit one mesh into the shadow depth buffer.
///
/// Slim version of `submit_draw`: model + light-VP + skinning + morph
/// only. No materials, no lights, no fog.
/// @param ctx_ptr Borrowed OpenGL backend context with an active shadow pass.
/// @param cmd Borrowed draw command supplying geometry, transform, alpha, and deformation state.
static void gl_shadow_draw(void *ctx_ptr, const vgfx3d_draw_cmd_t *cmd) {
    gl_context_t *ctx = (gl_context_t *)ctx_ptr;
    float safe_model[16];
    GLuint mesh_vbo, mesh_ibo;
    int mesh_compact = 0;
    uint32_t index_count;
    vgfx3d_draw_cmd_t safe_cmd;
    if (!ctx)
        return;
    if (!cmd || !cmd->vertices || !cmd->indices || ctx->shadow_pass_slot < 0 ||
        ctx->shadow_pass_slot >= VGFX3D_MAX_SHADOW_LIGHTS || cmd->vertex_count == 0 ||
        cmd->index_count == 0) {
        if (ctx->shadow_pass_slot >= 0)
            ctx->shadow_pass_failed = 1;
        return;
    }
    vgfx3d_sanitize_draw_command(cmd, &safe_cmd);
    cmd = &safe_cmd;
    index_count = vgfx3d_draw_cmd_validated_index_count(cmd);
    if (index_count == 0) {
        ctx->shadow_pass_failed = 1;
        return;
    }
    if (cmd->double_sided)
        gl.Disable(GL_CULL_FACE);
    else
        gl.Enable(GL_CULL_FACE);
    gl_apply_depth_bias(cmd, 0);
    if (!prepare_mesh_buffers(ctx, cmd, &mesh_vbo, &mesh_ibo, &mesh_compact)) {
        ctx->shadow_pass_failed = 1;
        return;
    }
    configure_mesh_attributes(ctx, mesh_vbo, mesh_ibo, mesh_compact);
    vgfx3d_copy_mat4_finite_or_identity(safe_model, cmd->model_matrix);
    gl.UniformMatrix4fv(ctx->shadow_uModelMatrix, 1, GL_TRUE, safe_model);
    gl.UniformMatrix4fv(
        ctx->shadow_uViewProjection, 1, GL_TRUE, ctx->shadow_vp[ctx->shadow_pass_slot]);
    bind_shadow_anim(ctx, cmd);
    {
        GLuint diffuse_tex = gl_get_material_texture(
            ctx,
            cmd->texture_asset,
            cmd->texture,
            cmd->texture_asset_cache_key[RT_MATERIAL3D_TEXTURE_SLOT_BASE_COLOR],
            cmd->texture_asset_mip_start[RT_MATERIAL3D_TEXTURE_SLOT_BASE_COLOR],
            cmd->texture_asset_mip_count[RT_MATERIAL3D_TEXTURE_SLOT_BASE_COLOR]);
        if (diffuse_tex == 0 && cmd->texture_required && vgfx3d_draw_cmd_has_base_texture(cmd))
            return; /* skipped in the main pass too; casting its shadow would be wrong */
        gl.Uniform1i(ctx->shadow_uHasTexture, diffuse_tex ? 1 : 0);
        gl.Uniform1i(ctx->shadow_uAlphaMode, vgfx3d_sanitize_alpha_mode(cmd->alpha_mode));
        gl.Uniform1f(ctx->shadow_uAlphaCutoff, cmd->alpha_cutoff);
        gl.Uniform1f(ctx->shadow_uAlpha, cmd->alpha);
        gl.Uniform4f(ctx->shadow_uDiffuseColor,
                     cmd->diffuse_color[0],
                     cmd->diffuse_color[1],
                     cmd->diffuse_color[2],
                     cmd->diffuse_color[3]);
        gl.Uniform4i(ctx->shadow_uTextureUvSets0,
                     vgfx3d_sanitize_texture_uv_set(cmd->texture_slot_uv_set[0]),
                     vgfx3d_sanitize_texture_uv_set(cmd->texture_slot_uv_set[1]),
                     vgfx3d_sanitize_texture_uv_set(cmd->texture_slot_uv_set[2]),
                     vgfx3d_sanitize_texture_uv_set(cmd->texture_slot_uv_set[3]));
        gl.Uniform4i(ctx->shadow_uTextureUvSets1,
                     vgfx3d_sanitize_texture_uv_set(cmd->texture_slot_uv_set[4]),
                     vgfx3d_sanitize_texture_uv_set(cmd->texture_slot_uv_set[5]),
                     0,
                     0);
        {
            float uv_transform0[RT_MATERIAL3D_TEXTURE_SLOT_COUNT][4];
            float uv_transform1[RT_MATERIAL3D_TEXTURE_SLOT_COUNT][4];
            for (int32_t tex_slot = 0; tex_slot < RT_MATERIAL3D_TEXTURE_SLOT_COUNT; tex_slot++) {
                uv_transform0[tex_slot][0] = cmd->texture_slot_uv_transform[tex_slot][0];
                uv_transform0[tex_slot][1] = cmd->texture_slot_uv_transform[tex_slot][1];
                uv_transform0[tex_slot][2] = cmd->texture_slot_uv_transform[tex_slot][2];
                uv_transform0[tex_slot][3] = cmd->texture_slot_uv_transform[tex_slot][3];
                uv_transform1[tex_slot][0] = cmd->texture_slot_uv_transform[tex_slot][4];
                uv_transform1[tex_slot][1] = cmd->texture_slot_uv_transform[tex_slot][5];
                uv_transform1[tex_slot][2] = 0.0f;
                uv_transform1[tex_slot][3] = 0.0f;
            }
            gl.Uniform4fv(ctx->shadow_uTextureUvTransform0,
                          RT_MATERIAL3D_TEXTURE_SLOT_COUNT,
                          &uv_transform0[0][0]);
            gl.Uniform4fv(ctx->shadow_uTextureUvTransform1,
                          RT_MATERIAL3D_TEXTURE_SLOT_COUNT,
                          &uv_transform1[0][0]);
        }
        bind_texture_unit_with_sampler(ctx,
                                       ctx->shadow_uDiffuseTex,
                                       0,
                                       GL_TEXTURE_2D,
                                       diffuse_tex,
                                       cmd,
                                       RT_MATERIAL3D_TEXTURE_SLOT_BASE_COLOR);
    }
    gl.DrawElements(GL_TRIANGLES, (GLsizei)index_count, GL_UNSIGNED_INT, NULL);
}

/// @brief Backend `shadow_end` op — exit shadow-pass mode and switch back to main.
///
/// Marks `shadow_active=1` so the main pass's `uShadowEnabled` uniform
/// turns shadow sampling on. Stores the bias the shader uses for
/// depth comparison. Re-binds the main FBO and main program for
/// subsequent draws.
/// @param ctx_ptr Borrowed OpenGL backend context whose shadow pass is finalized.
/// @param slot Shadow-light slot expected to own the active pass.
/// @param bias Depth-comparison bias stored for subsequent lighting draws.
static void gl_shadow_end(void *ctx_ptr, int32_t slot, float bias) {
    gl_context_t *ctx = (gl_context_t *)ctx_ptr;
    if (!ctx)
        return;
    if (slot < 0 || slot >= VGFX3D_MAX_SHADOW_LIGHTS || ctx->shadow_pass_slot != slot) {
        gl_leave_shadow_pass(ctx);
        return;
    }
    ctx->shadow_bias = vgfx3d_clamp_float_param(bias, -1000000.0f, 1000000.0f, 0.0f);
    ctx->shadow_complete[slot] =
        (!ctx->shadow_pass_failed && ctx->shadow_array_tex && ctx->shadow_fbo[slot] &&
         ctx->shadow_width[slot] > 0 && ctx->shadow_height[slot] > 0)
            ? 1
            : 0;
    gl_leave_shadow_pass(ctx);
}

/// @brief Backend `shadow_reuse` op — re-arm a slot from its previous-frame depth texture.
///
/// Per-slot shadow FBOs/textures persist across frames (recreated only on
/// resolution change), so an unchanged slot only needs its VP re-registered and
/// its complete flag restored — no clear, no rasterization.
/// @param ctx_ptr Borrowed OpenGL backend context containing persistent shadow textures.
/// @param slot Shadow slot to rearm.
/// @param depth_buf Ignored CPU buffer retained for backend-interface compatibility.
/// @param w Expected shadow texture width in pixels.
/// @param h Expected shadow texture height in pixels.
/// @param light_vp Borrowed replacement row-major light view-projection matrix.
/// @return 1 when the persistent slot is reusable, otherwise 0.
static int8_t gl_shadow_reuse(
    void *ctx_ptr, int32_t slot, float *depth_buf, int32_t w, int32_t h, const float *light_vp) {
    gl_context_t *ctx = (gl_context_t *)ctx_ptr;
    (void)depth_buf;
    if (!ctx || slot < 0 || slot >= VGFX3D_MAX_SHADOW_LIGHTS ||
        !vgfx3d_shadow_matrix_is_usable(light_vp) || w <= 0 || h <= 0)
        return 0;
    /* The shared array may be larger than this slot asked for (see
     * ensure_shadow_targets' grow-only policy), so accept any layer that covers it. */
    if (!ctx->shadow_array_tex || !ctx->shadow_fbo[slot] || ctx->shadow_width[slot] < w ||
        ctx->shadow_height[slot] < h)
        return 0;
    vgfx3d_copy_mat4_finite_or_identity(ctx->shadow_vp[slot], light_vp);
    ctx->shadow_complete[slot] = 1;
    gl_recompute_shadow_count(ctx);
    return 1;
}

/// @brief Backend `submit_draw_instanced` op — emit one `glDrawElementsInstanced`.
///
/// Same flow as `gl_submit_draw` plus per-instance matrix attributes
/// configured via `configure_instance_attributes`. After the draw,
/// resets the instance attributes to identity so subsequent non-
/// instanced draws don't accidentally see leftover instance data.
/// @param ctx_ptr Borrowed OpenGL backend context with an active draw target.
/// @param win Window handle retained for backend-interface compatibility; unused here.
/// @param cmd Borrowed mesh, material, particle, animation, and per-draw state.
/// @param instance_matrices Borrowed array of row-major model matrices, or NULL for particles.
/// @param instance_count Number of instances or compact particle records.
/// @param lights Borrowed light array; may be NULL when @p light_count is zero.
/// @param light_count Number of entries available through @p lights.
/// @param ambient Borrowed three-component ambient color; NULL is sanitized to black.
/// @param wireframe Nonzero to draw triangle edges instead of filled triangles.
/// @param backface_cull Nonzero to cull back-facing triangles.
static void gl_submit_draw_instanced(void *ctx_ptr,
                                     vgfx_window_t win,
                                     const vgfx3d_draw_cmd_t *cmd,
                                     const float *instance_matrices,
                                     int32_t instance_count,
                                     const vgfx3d_light_params_t *lights,
                                     int32_t light_count,
                                     const float *ambient,
                                     int8_t wireframe,
                                     int8_t backface_cull) {
    (void)win;
    gl_context_t *ctx = (gl_context_t *)ctx_ptr;
    GLuint mesh_vbo, mesh_ibo;
    GLint saved_uniform_locations[GL_MAIN_UNIFORM_SNAPSHOT_CAPACITY];
    GLuint saved_program = 0;
    int specialized_program;
    int mesh_compact = 0;
    int particle_mode;
    uint32_t index_count;
    vgfx3d_draw_cmd_t safe_cmd;
    vgfx3d_light_params_t safe_lights[VGFX3D_MAX_LIGHTS];
    float safe_ambient[3];
    if (!ctx || !cmd || !cmd->vertices || !cmd->indices || cmd->vertex_count == 0 ||
        cmd->index_count == 0 || instance_count <= 0 ||
        (!instance_matrices && !cmd->particle_instances)) {
        if (ctx)
            ctx->stats.dropped_draws++;
        return;
    }
    vgfx3d_sanitize_draw_command(cmd, &safe_cmd);
    cmd = &safe_cmd;
    light_count = vgfx3d_sanitize_light_array(lights, light_count, safe_lights, VGFX3D_MAX_LIGHTS);
    for (int32_t light_index = 0; light_index < light_count; light_index++)
        vgfx3d_sanitize_light_shadow_span(&safe_lights[light_index], ctx->shadow_count);
    lights = light_count > 0 ? safe_lights : NULL;
    vgfx3d_sanitize_ambient_rgb(ambient, safe_ambient);
    ambient = safe_ambient;
    particle_mode = cmd->particle_instances != NULL;
    index_count = vgfx3d_draw_cmd_validated_index_count(cmd);
    if (index_count == 0) {
        ctx->stats.dropped_draws++;
        return;
    }
    if (!prepare_mesh_buffers(ctx, cmd, &mesh_vbo, &mesh_ibo, &mesh_compact)) {
        ctx->stats.dropped_draws++;
        return;
    }

    gl.FrontFace(GL_CCW);
    if (backface_cull)
        gl.Enable(GL_CULL_FACE);
    else
        gl.Disable(GL_CULL_FACE);
    gl.PolygonMode(GL_FRONT_AND_BACK, wireframe ? GL_LINE : GL_FILL);
    gl_configure_draw_output(ctx, cmd);
    gl_apply_depth_bias(cmd, 1);

    specialized_program =
        gl_begin_material_program(ctx, cmd, saved_uniform_locations, &saved_program);
    gl.UseProgram(ctx->program);
    /* Resolve budgeted textures before IBL uniforms inspect native residency. */
    if (bind_material_textures(ctx, cmd) && cmd->texture_required) {
        gl_end_material_program(ctx, specialized_program, saved_uniform_locations, saved_program);
        return;
    }
    upload_main_uniforms(ctx, cmd, lights, light_count, ambient, particle_mode ? 0 : 1);
    configure_mesh_attributes(ctx, mesh_vbo, mesh_ibo, mesh_compact);
    if (!(particle_mode
              ? configure_particle_instance_attributes(ctx, cmd->particle_instances, instance_count)
              : configure_instance_attributes(
                    ctx,
                    instance_matrices,
                    cmd->has_prev_instance_matrices ? cmd->prev_instance_matrices : NULL,
                    instance_count))) {
        set_identity_instance_constants();
        ctx->stats.dropped_draws++;
        gl_end_material_program(ctx, specialized_program, saved_uniform_locations, saved_program);
        return;
    }
    gl.DrawElementsInstanced(
        GL_TRIANGLES, (GLsizei)index_count, GL_UNSIGNED_INT, NULL, (GLsizei)instance_count);
    gl_end_material_program(ctx, specialized_program, saved_uniform_locations, saved_program);
    ctx->stats.draw_calls++;
    set_identity_instance_constants();
}
