//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/backend/vgfx3d_backend_opengl_context.inc
// Purpose: OpenGL context creation, shader/program/object initialization, and teardown.
// Key invariants:
//   - Shader depth conventions are derived from the context being initialized, never global state.
//   - Context creation either publishes a complete resource set or releases every partial object.
// Ownership/Lifetime:
//   - This file is included by vgfx3d_backend_opengl.c and is not a standalone translation unit.
//   - GL objects and native bindings belong to gl_context_t until gl_destroy_ctx.
// Links: vgfx3d_backend_opengl.c, vgfx3d_backend_opengl_shaders.inc
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements OpenGL native-context creation, fixed resource setup, and teardown.
/// @details Context construction is transactional: it resolves a compatible native binding,
///          probes capabilities, compiles and links all shader programs, creates fixed GL
///          resources, and unwinds every completed stage when a later stage fails.

typedef struct gl_shader_set_t {
    GLuint vs;
    GLuint fs;
    GLuint unlit_fs;
    GLuint shadow_vs;
    GLuint shadow_fs;
    GLuint postfx_vs;
    GLuint postfx_fs;
    GLuint skybox_vs;
    GLuint skybox_fs;
    GLuint bloom_down_fs;
    GLuint bloom_up_fs;
    GLuint taa_fs;
    GLuint ssr_fs;
} gl_shader_set_t;

/// @brief Pick an FBConfig compatible with the target X window's visual/depth.
/// @param dpy Borrowed X display containing the target window.
/// @param xwin Target X window whose visual and depth must be matched.
/// @param configs Borrowed array of candidate framebuffer configurations.
/// @param fb_count Number of entries in @p configs.
/// @return The first exact visual/depth match, the first candidate as fallback, or NULL.
#if !defined(ZANNA_GRAPHICS_WAYLAND)
static GLXFBConfig gl_pick_matching_fb_config(Display *dpy,
                                              Window xwin,
                                              GLXFBConfig *configs,
                                              int fb_count) {
    GLXFBConfig chosen;
    XWindowAttributes window_attrs;
    if (!dpy || !configs || fb_count <= 0)
        return NULL;
    chosen = configs[0];
    if (XGetWindowAttributes(dpy, xwin, &window_attrs)) {
        for (int i = 0; i < fb_count; ++i) {
            XVisualInfo *visual_info = glx.GetVisualFromFBConfig(dpy, configs[i]);
            if (!visual_info)
                continue;
            const int visual_match = visual_info->visual == window_attrs.visual;
            const int depth_match = visual_info->depth == window_attrs.depth;
            XFree(visual_info);
            if (visual_match && depth_match) {
                chosen = configs[i];
                break;
            }
        }
    }
    return chosen;
}
#endif

/// @brief Allocate the GL_TIMESTAMP query ring used for GPU frame timing (GAP-9).
/// @details Requires all five ARB_timer_query entry points; any missing one leaves
///   `timer_queries_ready` clear and `Canvas3D.FrameGpuTimeUs` reading 0, matching the
///   documented "zero before a result is available" contract rather than trapping.
/// @param ctx Borrowed OpenGL context with a current GL context; NULL is ignored.
static void gl_init_timer_queries(gl_context_t *ctx) {
    if (!ctx)
        return;
    ctx->timer_slot_active = -1;
    if (!gl.GenQueries || !gl.DeleteQueries || !gl.QueryCounter || !gl.GetQueryObjectiv ||
        !gl.GetQueryObjectui64v)
        return;
    gl.GenQueries((GLsizei)(VGFX3D_GL_TIMER_FRAMES * 2), &ctx->timer_query[0][0]);
    if (!gl_drain_errors("timer query alloc") || ctx->timer_query[0][0] == 0)
        return;
    ctx->timer_queries_ready = 1;
}

/// @brief Release the GPU timing query ring.
/// @param ctx Borrowed OpenGL context with a current GL context; NULL is ignored.
static void gl_destroy_timer_queries(gl_context_t *ctx) {
    if (!ctx || !ctx->timer_queries_ready || !gl.DeleteQueries)
        return;
    gl.DeleteQueries((GLsizei)(VGFX3D_GL_TIMER_FRAMES * 2), &ctx->timer_query[0][0]);
    memset(ctx->timer_query, 0, sizeof(ctx->timer_query));
    memset(ctx->timer_query_pending, 0, sizeof(ctx->timer_query_pending));
    ctx->timer_queries_ready = 0;
    ctx->timer_slot_active = -1;
}

/// @brief Collect any retired GPU timestamp pairs into `frame_gpu_time_us`.
/// @details Polls with GL_QUERY_RESULT_AVAILABLE and never blocks: an unfinished pair is
///   left pending for a later call, so the reported figure trails real time by up to
///   `VGFX3D_GL_TIMER_FRAMES` frames instead of stalling the pipeline being measured.
///   Slots are walked oldest-first so the retained value is the most recent completed frame.
/// @param ctx Borrowed OpenGL context with a current GL context; NULL is ignored.
static void gl_harvest_timer_queries(gl_context_t *ctx) {
    if (!ctx || !ctx->timer_queries_ready)
        return;
    for (int32_t step = 0; step < VGFX3D_GL_TIMER_FRAMES; step++) {
        int32_t slot = (ctx->timer_query_head + step) % VGFX3D_GL_TIMER_FRAMES;
        GLint available = 0;
        uint64_t start_ns = 0;
        uint64_t end_ns = 0;
        if (!ctx->timer_query_pending[slot])
            continue;
        gl.GetQueryObjectiv(ctx->timer_query[slot][1], GL_QUERY_RESULT_AVAILABLE, &available);
        if (!available)
            continue;
        gl.GetQueryObjectui64v(ctx->timer_query[slot][0], GL_QUERY_RESULT, &start_ns);
        gl.GetQueryObjectui64v(ctx->timer_query[slot][1], GL_QUERY_RESULT, &end_ns);
        ctx->timer_query_pending[slot] = 0;
        /* Timestamps come from a monotonic GPU counter, but a context loss or driver
         * reset can retire them out of order — publish only a forward-going delta. */
        if (end_ns > start_ns)
            ctx->frame_gpu_time_us = (end_ns - start_ns) / 1000u;
    }
    (void)gl_drain_errors("timer query harvest");
}

/// @brief Initialize all per-context matrices and default target/upload state.
/// @param ctx Borrowed newly allocated OpenGL context to initialize.
/// @param w Initial logical output width in pixels.
/// @param h Initial logical output height in pixels.
static void gl_init_context_defaults(gl_context_t *ctx, int32_t w, int32_t h) {
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
    ctx->width = w;
    ctx->height = h;
    ctx->render_scale = 1.0f;
    memcpy(ctx->view, kIdentity4x4, sizeof(ctx->view));
    memcpy(ctx->projection, kIdentity4x4, sizeof(ctx->projection));
    memcpy(ctx->vp, kIdentity4x4, sizeof(ctx->vp));
    memcpy(ctx->inv_vp, kIdentity4x4, sizeof(ctx->inv_vp));
    memcpy(ctx->draw_prev_vp, kIdentity4x4, sizeof(ctx->draw_prev_vp));
    memcpy(ctx->scene_vp, kIdentity4x4, sizeof(ctx->scene_vp));
    memcpy(ctx->scene_prev_vp, kIdentity4x4, sizeof(ctx->scene_prev_vp));
    memcpy(ctx->scene_inv_vp, kIdentity4x4, sizeof(ctx->scene_inv_vp));
    ctx->active_target_kind = VGFX3D_OPENGL_TARGET_SWAPCHAIN;
    ctx->texture_upload_budget_bytes = UINT64_MAX;
    ctx->default_doublebuffered = 1;
    ctx->default_draw_buffer = GL_BACK;
    ctx->present_path = 0;
}

/// @brief Cache the default framebuffer draw/read buffer for this GLX context.
/// @details Linux windows may be created by a shared X11 layer before the 3D backend is selected.
///          Some drivers expose the resulting drawable as single-buffered even when a
///          double-buffered FBConfig was requested. In that case `GL_BACK` draws are discarded or
///          unreadable, producing a blank client area; targeting `GL_FRONT` keeps presentation and
///          screenshots tied to the drawable that actually exists.
/// @param ctx Borrowed current OpenGL context receiving the default buffer selection.
static void gl_query_default_framebuffer_buffer(gl_context_t *ctx) {
    GLint doublebuffered = 1;
    if (!ctx || !gl.GetIntegerv)
        return;
    gl.GetIntegerv(GL_DOUBLEBUFFER, &doublebuffered);
    ctx->default_doublebuffered = doublebuffered ? 1 : 0;
    ctx->default_draw_buffer = ctx->default_doublebuffered ? GL_BACK : GL_FRONT;
    if (gl_debug_enabled()) {
        fprintf(stderr,
                "[OpenGL] default framebuffer doublebuffered=%d draw_buffer=0x%04X\n",
                (int)ctx->default_doublebuffered,
                (unsigned)ctx->default_draw_buffer);
    }
}

/// @brief Detect whether the GLX default framebuffer accepts RGB writes.
/// @details Some Linux GLX/X11 combinations create a context successfully but silently keep the
///          window drawable's RGB channels black. Offscreen FBO rendering still works in that
///          configuration, so the backend can fall back to presenting the final offscreen image
///          through ZannaGFX. The probe saves and restores framebuffer state and only runs during
///          context creation before user-visible rendering begins.
/// @param ctx Borrowed current OpenGL context receiving the probe result.
static void gl_probe_default_framebuffer_writable(gl_context_t *ctx) {
    gl_framebuffer_state_t saved;
    uint8_t sample[4] = {0, 0, 0, 0};
    if (!ctx || !gl.ReadPixels)
        return;
    gl_capture_framebuffer_state(&saved);
    gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
    gl.DrawBuffer(ctx->default_draw_buffer);
    gl.ReadBuffer(ctx->default_draw_buffer);
    gl.Viewport(0, 0, ctx->width, ctx->height);
    gl.Disable(GL_SCISSOR_TEST);
    gl.ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    gl.ClearColor(1.0f, 0.0f, 0.0f, 1.0f);
    gl.Clear(GL_COLOR_BUFFER_BIT);
    gl.ReadPixels(ctx->width / 2, ctx->height / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, sample);
    ctx->default_framebuffer_writable = sample[0] > 200 && sample[1] < 32 && sample[2] < 32;
    if (gl_debug_enabled()) {
        fprintf(stderr,
                "[OpenGL] default framebuffer writable=%d sample=%u,%u,%u,%u\n",
                (int)ctx->default_framebuffer_writable,
                (unsigned)sample[0],
                (unsigned)sample[1],
                (unsigned)sample[2],
                (unsigned)sample[3]);
    }
    gl_restore_framebuffer_state(&saved);
}

/// @brief Compile every shader used by the OpenGL backend context.
/// @param[out] shaders Caller-owned temporary bundle receiving compiled shader object names.
/// @param zero_to_one_clip Nonzero to compile shaders for zero-to-one clip-space depth.
/// @return Nonzero only when every required shader compiles successfully.
static int gl_compile_context_shaders(gl_shader_set_t *shaders, int zero_to_one_clip) {
    memset(shaders, 0, sizeof(*shaders));
    shaders->vs =
        compile_shader_parts(GL_VERTEX_SHADER,
                             glsl_vertex_src,
                             (GLsizei)(sizeof(glsl_vertex_src) / sizeof(glsl_vertex_src[0])),
                             zero_to_one_clip);
    shaders->fs =
        compile_shader_parts(GL_FRAGMENT_SHADER,
                             glsl_fragment_src,
                             (GLsizei)(sizeof(glsl_fragment_src) / sizeof(glsl_fragment_src[0])),
                             zero_to_one_clip);
    shaders->unlit_fs = compile_shader_parts(
        GL_FRAGMENT_SHADER,
        glsl_unlit_fragment_src,
        (GLsizei)(sizeof(glsl_unlit_fragment_src) / sizeof(glsl_unlit_fragment_src[0])),
        zero_to_one_clip);
    shaders->shadow_vs = compile_shader(GL_VERTEX_SHADER, glsl_shadow_vertex_src, zero_to_one_clip);
    shaders->shadow_fs =
        compile_shader(GL_FRAGMENT_SHADER, glsl_shadow_fragment_src, zero_to_one_clip);
    shaders->postfx_vs = compile_shader(GL_VERTEX_SHADER, glsl_postfx_vertex_src, zero_to_one_clip);
    shaders->postfx_fs = compile_shader_parts(
        GL_FRAGMENT_SHADER,
        glsl_postfx_fragment_src,
        (GLsizei)(sizeof(glsl_postfx_fragment_src) / sizeof(glsl_postfx_fragment_src[0])),
        zero_to_one_clip);
    shaders->skybox_vs = compile_shader(GL_VERTEX_SHADER, glsl_skybox_vertex_src, zero_to_one_clip);
    shaders->skybox_fs =
        compile_shader(GL_FRAGMENT_SHADER, glsl_skybox_fragment_src, zero_to_one_clip);
    shaders->bloom_down_fs =
        compile_shader(GL_FRAGMENT_SHADER, glsl_bloom_down_fragment_src, zero_to_one_clip);
    shaders->bloom_up_fs =
        compile_shader(GL_FRAGMENT_SHADER, glsl_bloom_up_fragment_src, zero_to_one_clip);
    shaders->taa_fs = compile_shader(GL_FRAGMENT_SHADER, glsl_taa_fragment_src, zero_to_one_clip);
    shaders->ssr_fs = compile_shader(GL_FRAGMENT_SHADER, glsl_ssr_fragment_src, zero_to_one_clip);
    return shaders->vs && shaders->fs && shaders->unlit_fs && shaders->shadow_vs &&
           shaders->shadow_fs && shaders->postfx_vs && shaders->postfx_fs && shaders->skybox_vs &&
           shaders->skybox_fs && shaders->bloom_down_fs && shaders->bloom_up_fs &&
           shaders->taa_fs && shaders->ssr_fs;
}

/// @brief Delete all shaders in a temporary context shader bundle.
/// @param[in,out] shaders Caller-owned bundle whose nonzero shader objects are deleted and cleared.
static void gl_delete_context_shaders(gl_shader_set_t *shaders) {
    if (shaders->vs)
        gl.DeleteShader(shaders->vs);
    if (shaders->fs)
        gl.DeleteShader(shaders->fs);
    if (shaders->unlit_fs)
        gl.DeleteShader(shaders->unlit_fs);
    if (shaders->shadow_vs)
        gl.DeleteShader(shaders->shadow_vs);
    if (shaders->shadow_fs)
        gl.DeleteShader(shaders->shadow_fs);
    if (shaders->postfx_vs)
        gl.DeleteShader(shaders->postfx_vs);
    if (shaders->postfx_fs)
        gl.DeleteShader(shaders->postfx_fs);
    if (shaders->skybox_vs)
        gl.DeleteShader(shaders->skybox_vs);
    if (shaders->skybox_fs)
        gl.DeleteShader(shaders->skybox_fs);
    if (shaders->bloom_down_fs)
        gl.DeleteShader(shaders->bloom_down_fs);
    if (shaders->bloom_up_fs)
        gl.DeleteShader(shaders->bloom_up_fs);
    if (shaders->taa_fs)
        gl.DeleteShader(shaders->taa_fs);
    memset(shaders, 0, sizeof(*shaders));
}

/// @brief Link shader programs from the temporary shader bundle.
/// @param ctx Borrowed OpenGL context receiving all linked program object names.
/// @param shaders Borrowed bundle containing the successfully compiled shader stages.
/// @return Nonzero only when every required program links successfully.
static int gl_link_context_programs(gl_context_t *ctx, const gl_shader_set_t *shaders) {
    ctx->program = link_program(shaders->vs, shaders->fs);
    ctx->unlit_program = link_program(shaders->vs, shaders->unlit_fs);
    ctx->shadow_program = link_program(shaders->shadow_vs, shaders->shadow_fs);
    ctx->postfx_program = link_program(shaders->postfx_vs, shaders->postfx_fs);
    ctx->skybox_program = link_program(shaders->skybox_vs, shaders->skybox_fs);
    /* Plan 05 pass programs share the fullscreen post-FX vertex shader. */
    ctx->bloom_down_program = link_program(shaders->postfx_vs, shaders->bloom_down_fs);
    ctx->bloom_up_program = link_program(shaders->postfx_vs, shaders->bloom_up_fs);
    ctx->taa_program = link_program(shaders->postfx_vs, shaders->taa_fs);
    ctx->ssr_program = link_program(shaders->postfx_vs, shaders->ssr_fs);
    return ctx->program && ctx->unlit_program && ctx->shadow_program && ctx->postfx_program &&
           ctx->skybox_program && ctx->bloom_down_program && ctx->bloom_up_program &&
           ctx->taa_program && ctx->ssr_program;
}

/// @brief Delete shader programs created during context setup.
/// @param ctx Borrowed OpenGL context whose nonzero program objects are deleted and cleared.
static void gl_delete_context_programs(gl_context_t *ctx) {
    if (ctx->program)
        gl.DeleteProgram(ctx->program);
    if (ctx->unlit_program)
        gl.DeleteProgram(ctx->unlit_program);
    if (ctx->shadow_program)
        gl.DeleteProgram(ctx->shadow_program);
    if (ctx->postfx_program)
        gl.DeleteProgram(ctx->postfx_program);
    if (ctx->skybox_program)
        gl.DeleteProgram(ctx->skybox_program);
    if (ctx->bloom_down_program)
        gl.DeleteProgram(ctx->bloom_down_program);
    if (ctx->bloom_up_program)
        gl.DeleteProgram(ctx->bloom_up_program);
    if (ctx->taa_program)
        gl.DeleteProgram(ctx->taa_program);
    if (ctx->ssr_program)
        gl.DeleteProgram(ctx->ssr_program);
    ctx->program = 0;
    ctx->unlit_program = 0;
    ctx->shadow_program = 0;
    ctx->postfx_program = 0;
    ctx->skybox_program = 0;
    ctx->bloom_down_program = 0;
    ctx->bloom_up_program = 0;
    ctx->taa_program = 0;
    ctx->ssr_program = 0;
}

/// @brief Bind default sampler-unit uniforms across main, shadow, skybox and post-FX programs.
/// @param ctx Borrowed OpenGL context whose current main program receives fixed texture units.
static void gl_bind_main_sampler_uniforms(gl_context_t *ctx) {
    gl.UseProgram(ctx->program);
    gl.Uniform1i(ctx->uDiffuseTex, 0);
    gl.Uniform1i(ctx->uNormalTex, 1);
    gl.Uniform1i(ctx->uSpecularTex, 2);
    gl.Uniform1i(ctx->uEmissiveTex, 3);
    gl.Uniform1i(ctx->uShadowArray, GL_TU_SHADOW_ARRAY);
    gl.Uniform1i(ctx->uSplatTex, GL_TU_SPLAT_CONTROL);
    gl.Uniform1i(ctx->uSplatLayer0, GL_TU_SPLAT_LAYER0);
    gl.Uniform1i(ctx->uSplatLayer1, GL_TU_SPLAT_LAYER0 + 1);
    gl.Uniform1i(ctx->uSplatLayer2, GL_TU_SPLAT_LAYER0 + 2);
    gl.Uniform1i(ctx->uSplatLayer3, GL_TU_SPLAT_LAYER0 + 3);
    gl.Uniform1i(ctx->uMorphDeltas, GL_TU_MORPH_DELTAS);
    gl.Uniform1i(ctx->uMorphNormalDeltas, GL_TU_MORPH_NORMAL_DELTAS);
    gl.Uniform1i(ctx->uEnvMap, GL_TU_ENV_MAP);
    gl.Uniform1i(ctx->uBrdfLut, GL_TU_BRDF_LUT);
    gl.Uniform1i(ctx->uMetallicRoughnessTex, GL_TU_METALLIC_ROUGHNESS);
    gl.Uniform1i(ctx->uAOTex, GL_TU_AO);
}

/// @brief Bind fixed sampler-unit assignments for every context-owned shader program.
/// @param ctx Borrowed OpenGL context containing linked programs and uniform locations.
static void gl_bind_default_sampler_uniforms(gl_context_t *ctx) {
    gl_bind_main_sampler_uniforms(ctx);
    gl.UseProgram(ctx->shadow_program);
    gl.Uniform1i(ctx->shadow_uMorphDeltas, GL_TU_MORPH_DELTAS);
    gl.Uniform1i(ctx->shadow_uDiffuseTex, 0);

    gl.UseProgram(ctx->skybox_program);
    gl.Uniform1i(ctx->skybox_uSkybox, GL_TU_ENV_MAP);

    gl.UseProgram(ctx->postfx_program);
    gl.Uniform1i(ctx->postfx_uSceneTex, 0);
    gl.Uniform1i(ctx->postfx_uSceneDepthTex, 1);
    gl.Uniform1i(ctx->postfx_uSceneMotionTex, 2);
    gl.Uniform1i(ctx->postfx_uBloomTex, 3);
    gl.Uniform1i(ctx->postfx_uLutTex, 4);

    gl.UseProgram(ctx->bloom_down_program);
    gl.Uniform1i(ctx->bloom_down_uSrcTex, 0);
    gl.UseProgram(ctx->bloom_up_program);
    gl.Uniform1i(ctx->bloom_up_uSrcTex, 0);
    gl.UseProgram(ctx->taa_program);
    gl.Uniform1i(ctx->taa_uCurrTex, 0);
    gl.Uniform1i(ctx->taa_uHistTex, 1);
    gl.Uniform1i(ctx->taa_uMotionTex, 2);
    gl.Uniform1i(ctx->taa_uDepthTex, 3);
}

/// @brief Bind uniform-block indices for current/previous bone palettes.
/// @param ctx Borrowed OpenGL context whose programs receive fixed UBO binding points.
static void gl_bind_context_uniform_blocks(gl_context_t *ctx) {
    GLuint main_block = gl.GetUniformBlockIndex(ctx->program, "Bones");
    GLuint prev_main_block = gl.GetUniformBlockIndex(ctx->program, "PrevBones");
    GLuint unlit_block = gl.GetUniformBlockIndex(ctx->unlit_program, "Bones");
    GLuint prev_unlit_block = gl.GetUniformBlockIndex(ctx->unlit_program, "PrevBones");
    GLuint shadow_block = gl.GetUniformBlockIndex(ctx->shadow_program, "Bones");
    if (main_block != GL_INVALID_INDEX)
        gl.UniformBlockBinding(ctx->program, main_block, 0);
    if (prev_main_block != GL_INVALID_INDEX)
        gl.UniformBlockBinding(ctx->program, prev_main_block, 1);
    if (unlit_block != GL_INVALID_INDEX)
        gl.UniformBlockBinding(ctx->unlit_program, unlit_block, 0);
    if (prev_unlit_block != GL_INVALID_INDEX)
        gl.UniformBlockBinding(ctx->unlit_program, prev_unlit_block, 1);
    if (shadow_block != GL_INVALID_INDEX)
        gl.UniformBlockBinding(ctx->shadow_program, shadow_block, 0);
    {
        GLuint offsets_block = gl.GetUniformBlockIndex(ctx->program, "ClusterOffsets");
        GLuint indices_block = gl.GetUniformBlockIndex(ctx->program, "ClusterIndices");
        if (offsets_block != GL_INVALID_INDEX)
            gl.UniformBlockBinding(ctx->program, offsets_block, 2);
        if (indices_block != GL_INVALID_INDEX)
            gl.UniformBlockBinding(ctx->program, indices_block, 3);
    }
}

/// @brief Generate VAOs, buffers, UBOs, and morph texture-buffer handles for the context.
/// @param ctx Borrowed OpenGL context receiving the generated object names.
/// @return Non-zero when every mandatory GL object name was created successfully.
static int gl_generate_context_objects(gl_context_t *ctx) {
    if (!ctx)
        return 0;
    gl.GenVertexArrays(1, &ctx->vao);
    gl.GenVertexArrays(1, &ctx->fullscreen_vao);
    gl.GenVertexArrays(1, &ctx->skybox_vao);
    gl.GenBuffers(1, &ctx->fullscreen_vbo);
    gl.GenBuffers(1, &ctx->mesh_vbo);
    gl.GenBuffers(1, &ctx->mesh_ibo);
    gl.GenBuffers(1, &ctx->instance_vbo);
    gl.GenBuffers(1, &ctx->prev_instance_vbo);
    gl.GenBuffers(1, &ctx->bone_ubo);
    gl.GenBuffers(1, &ctx->prev_bone_ubo);
    gl.GenBuffers(1, &ctx->cluster_offsets_ubo);
    gl.GenBuffers(1, &ctx->cluster_indices_ubo);
    gl.GenBuffers(1, &ctx->morph_buffer);
    gl.GenBuffers(1, &ctx->morph_normal_buffer);
    gl.GenBuffers(1, &ctx->skybox_vbo);
    gl.GenTextures(1, &ctx->morph_tbo);
    gl.GenTextures(1, &ctx->morph_normal_tbo);
    return ctx->vao && ctx->fullscreen_vao && ctx->skybox_vao && ctx->fullscreen_vbo &&
           ctx->mesh_vbo && ctx->mesh_ibo && ctx->instance_vbo && ctx->prev_instance_vbo &&
           ctx->bone_ubo && ctx->prev_bone_ubo && ctx->morph_buffer && ctx->morph_normal_buffer &&
           ctx->skybox_vbo && ctx->morph_tbo && ctx->morph_normal_tbo && gl_upload_ok();
}

/// @brief Allocate the dynamic streaming VBO/IBO, instance, morph, and bone buffers.
/// @param ctx Borrowed OpenGL context whose generated buffers receive initial storage.
/// @return Non-zero when all initial storage allocations complete without GL errors.
static int gl_init_streaming_buffers(gl_context_t *ctx) {
    if (!ctx)
        return 0;
    gl.BindBuffer(GL_ARRAY_BUFFER, ctx->mesh_vbo);
    gl.BufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(4 * 1024 * 1024), NULL, GL_STREAM_DRAW);
    ctx->mesh_vbo_capacity = 4u * 1024u * 1024u;
    gl.BindBuffer(GL_ELEMENT_ARRAY_BUFFER, ctx->mesh_ibo);
    gl.BufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)(1 * 1024 * 1024), NULL, GL_STREAM_DRAW);
    ctx->mesh_ibo_capacity = 1u * 1024u * 1024u;
    gl.BindBuffer(GL_ARRAY_BUFFER, ctx->instance_vbo);
    gl.BufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(64 * 1024), NULL, GL_STREAM_DRAW);
    ctx->instance_vbo_capacity = 64u * 1024u;
    gl.BindBuffer(GL_ARRAY_BUFFER, ctx->prev_instance_vbo);
    gl.BufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(64 * 1024), NULL, GL_STREAM_DRAW);
    ctx->prev_instance_vbo_capacity = 64u * 1024u;
    gl.BindBuffer(GL_TEXTURE_BUFFER, ctx->morph_buffer);
    gl.BufferData(GL_TEXTURE_BUFFER, (GLsizeiptr)(64 * 1024), NULL, GL_STREAM_DRAW);
    ctx->morph_capacity_bytes = 64u * 1024u;
    gl.BindBuffer(GL_TEXTURE_BUFFER, ctx->morph_normal_buffer);
    gl.BufferData(GL_TEXTURE_BUFFER, (GLsizeiptr)(64 * 1024), NULL, GL_STREAM_DRAW);
    ctx->morph_normal_capacity_bytes = 64u * 1024u;

    gl.BindBuffer(GL_UNIFORM_BUFFER, ctx->bone_ubo);
    gl.BufferData(
        GL_UNIFORM_BUFFER, (GLsizeiptr)VGFX3D_OPENGL_BONE_PALETTE_BYTES, NULL, GL_DYNAMIC_DRAW);
    gl.BindBufferBase(GL_UNIFORM_BUFFER, 0, ctx->bone_ubo);
    gl.BindBuffer(GL_UNIFORM_BUFFER, ctx->prev_bone_ubo);
    gl.BufferData(
        GL_UNIFORM_BUFFER, (GLsizeiptr)VGFX3D_OPENGL_BONE_PALETTE_BYTES, NULL, GL_DYNAMIC_DRAW);
    gl.BindBufferBase(GL_UNIFORM_BUFFER, 1, ctx->prev_bone_ubo);
    gl.BindBuffer(GL_UNIFORM_BUFFER, ctx->cluster_offsets_ubo);
    gl.BufferData(GL_UNIFORM_BUFFER,
                  (GLsizeiptr)((sizeof(((vgfx3d_cluster_table_t *)0)->offsets) + 15u) & ~15u),
                  NULL,
                  GL_DYNAMIC_DRAW);
    gl.BindBufferBase(GL_UNIFORM_BUFFER, 2, ctx->cluster_offsets_ubo);
    gl.BindBuffer(GL_UNIFORM_BUFFER, ctx->cluster_indices_ubo);
    gl.BufferData(GL_UNIFORM_BUFFER,
                  (GLsizeiptr)((sizeof(((vgfx3d_cluster_table_t *)0)->indices) + 15u) & ~15u),
                  NULL,
                  GL_DYNAMIC_DRAW);
    gl.BindBufferBase(GL_UNIFORM_BUFFER, 3, ctx->cluster_indices_ubo);
    return gl_upload_ok();
}

/// @brief Upload immutable fullscreen-triangle and skybox vertex buffers.
/// @param ctx Borrowed OpenGL context containing the helper VAOs and VBOs.
/// @return Non-zero when both immutable helper meshes upload and attribute setup succeeds.
static int gl_init_static_geometry_buffers(gl_context_t *ctx) {
    if (!ctx)
        return 0;
    gl.BindVertexArray(ctx->fullscreen_vao);
    gl.BindBuffer(GL_ARRAY_BUFFER, ctx->fullscreen_vbo);
    gl.BufferData(GL_ARRAY_BUFFER,
                  (GLsizeiptr)sizeof(gl_fullscreen_triangle),
                  gl_fullscreen_triangle,
                  GL_STATIC_DRAW);
    gl.VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * (GLsizei)sizeof(float), (void *)0);
    gl.EnableVertexAttribArray(0);

    gl.BindVertexArray(ctx->skybox_vao);
    gl.BindBuffer(GL_ARRAY_BUFFER, ctx->skybox_vbo);
    gl.BufferData(GL_ARRAY_BUFFER,
                  (GLsizeiptr)sizeof(gl_skybox_vertices),
                  gl_skybox_vertices,
                  GL_STATIC_DRAW);
    gl.VertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * (GLsizei)sizeof(float), (void *)0);
    gl.EnableVertexAttribArray(0);
    return gl_upload_ok();
}

/// @brief Create the context-owned 1x1 white fallback texture.
///
/// Budgeted texture uploads can take more than one frame. Binding this texture
/// while the real texture is pending keeps the shader's textured path active
/// and multiplies by white, so authored material colors remain visible without
/// introducing a black/transparent first-frame artifact.
/// @param ctx Borrowed OpenGL context that owns the fallback texture.
/// @return Non-zero when the fallback texture is available.
static int gl_create_default_white_texture(gl_context_t *ctx) {
    static const uint8_t kWhitePixel[4] = {255u, 255u, 255u, 255u};

    if (!ctx)
        return 0;
    if (ctx->default_white_tex)
        return 1;
    gl.GenTextures(1, &ctx->default_white_tex);
    if (!ctx->default_white_tex)
        return 0;
    gl.BindTexture(GL_TEXTURE_2D, ctx->default_white_tex);
    gl.PixelStorei(GL_UNPACK_ALIGNMENT, 1);
    gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, kWhitePixel);
    gl.PixelStorei(GL_UNPACK_ALIGNMENT, 4);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    if (!gl_upload_ok()) {
        gl.DeleteTextures(1, &ctx->default_white_tex);
        ctx->default_white_tex = 0;
        return 0;
    }
    return 1;
}

/// @brief Create the context-owned split-sum environment-BRDF lookup texture
/// from the shared CPU precomputation (RG32F, clamped bilinear).
/// @param ctx Borrowed OpenGL context that owns the lookup texture.
/// @return Non-zero when the table texture is available.
static int gl_create_brdf_lut_texture(gl_context_t *ctx) {
    if (!ctx)
        return 0;
    if (ctx->brdf_lut_tex)
        return 1;
    gl.GenTextures(1, &ctx->brdf_lut_tex);
    if (!ctx->brdf_lut_tex)
        return 0;
    gl.BindTexture(GL_TEXTURE_2D, ctx->brdf_lut_tex);
    gl.TexImage2D(GL_TEXTURE_2D,
                  0,
                  GL_RG32F,
                  VGFX3D_BRDF_LUT_SIZE,
                  VGFX3D_BRDF_LUT_SIZE,
                  0,
                  GL_RG,
                  GL_FLOAT,
                  vgfx3d_brdf_lut_data());
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    if (!gl_upload_ok()) {
        gl.DeleteTextures(1, &ctx->brdf_lut_tex);
        ctx->brdf_lut_tex = 0;
        return 0;
    }
    return 1;
}

/// @brief Create the context-owned 1x1 white fallback cubemap.
///
/// Budgeted cubemap uploads reserve a texture object before all six faces have
/// valid pixels. This fallback provides a complete cubemap for those interim
/// frames, avoiding invalid binds and incomplete-cubemap sampling during
/// environment-map streaming.
/// @param ctx Borrowed OpenGL context that owns the fallback cubemap.
/// @return Non-zero when the fallback cubemap is available.
static int gl_create_default_white_cubemap(gl_context_t *ctx) {
    static const uint8_t kWhitePixel[4] = {255u, 255u, 255u, 255u};

    if (!ctx)
        return 0;
    if (ctx->default_white_cubemap)
        return 1;
    gl.GenTextures(1, &ctx->default_white_cubemap);
    if (!ctx->default_white_cubemap)
        return 0;
    gl.BindTexture(GL_TEXTURE_CUBE_MAP, ctx->default_white_cubemap);
    gl.PixelStorei(GL_UNPACK_ALIGNMENT, 1);
    for (int face = 0; face < 6; face++) {
        gl.TexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + (GLenum)face,
                      0,
                      GL_RGBA8,
                      1,
                      1,
                      0,
                      GL_RGBA,
                      GL_UNSIGNED_BYTE,
                      kWhitePixel);
    }
    gl.PixelStorei(GL_UNPACK_ALIGNMENT, 4);
    gl.TexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl.TexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl.TexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    gl.TexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl.TexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl.TexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_BASE_LEVEL, 0);
    gl.TexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAX_LEVEL, 0);
    if (!gl_upload_ok()) {
        gl.DeleteTextures(1, &ctx->default_white_cubemap);
        ctx->default_white_cubemap = 0;
        return 0;
    }
    return 1;
}

/// @brief Release fixed context objects created before full backend initialization succeeds.
///
/// This is the failure-path counterpart to the tail of `gl_destroy_ctx()`. It intentionally skips
/// caches and render targets because they are created only after `create_ctx()` returns a live
/// backend context.
/// @param ctx Borrowed OpenGL context whose fixed GL objects are deleted and zeroed.
static void gl_release_fixed_context_objects(gl_context_t *ctx) {
    if (!ctx)
        return;
    if (ctx->fullscreen_vbo)
        gl.DeleteBuffers(1, &ctx->fullscreen_vbo);
    if (ctx->skybox_vbo)
        gl.DeleteBuffers(1, &ctx->skybox_vbo);
    if (ctx->depth_probe_fence)
        gl.DeleteSync(ctx->depth_probe_fence);
    if (ctx->depth_probe_pbo)
        gl.DeleteBuffers(1, &ctx->depth_probe_pbo);
    if (ctx->default_white_tex)
        gl.DeleteTextures(1, &ctx->default_white_tex);
    if (ctx->brdf_lut_tex)
        gl.DeleteTextures(1, &ctx->brdf_lut_tex);
    if (ctx->default_white_cubemap)
        gl.DeleteTextures(1, &ctx->default_white_cubemap);
    if (ctx->morph_normal_tbo)
        gl.DeleteTextures(1, &ctx->morph_normal_tbo);
    if (ctx->morph_tbo)
        gl.DeleteTextures(1, &ctx->morph_tbo);
    if (ctx->morph_normal_buffer)
        gl.DeleteBuffers(1, &ctx->morph_normal_buffer);
    if (ctx->bone_ubo)
        gl.DeleteBuffers(1, &ctx->bone_ubo);
    if (ctx->prev_bone_ubo)
        gl.DeleteBuffers(1, &ctx->prev_bone_ubo);
    if (ctx->cluster_offsets_ubo)
        gl.DeleteBuffers(1, &ctx->cluster_offsets_ubo);
    if (ctx->cluster_indices_ubo)
        gl.DeleteBuffers(1, &ctx->cluster_indices_ubo);
    if (ctx->opaque_depth_tex)
        gl.DeleteTextures(1, &ctx->opaque_depth_tex);
    if (ctx->opaque_depth_fbo)
        gl.DeleteFramebuffers(1, &ctx->opaque_depth_fbo);
    if (ctx->morph_buffer)
        gl.DeleteBuffers(1, &ctx->morph_buffer);
    if (ctx->instance_vbo)
        gl.DeleteBuffers(1, &ctx->instance_vbo);
    if (ctx->prev_instance_vbo)
        gl.DeleteBuffers(1, &ctx->prev_instance_vbo);
    if (ctx->mesh_vbo)
        gl.DeleteBuffers(1, &ctx->mesh_vbo);
    if (ctx->mesh_ibo)
        gl.DeleteBuffers(1, &ctx->mesh_ibo);
    if (ctx->skybox_vao)
        gl.DeleteVertexArrays(1, &ctx->skybox_vao);
    if (ctx->fullscreen_vao)
        gl.DeleteVertexArrays(1, &ctx->fullscreen_vao);
    if (ctx->vao)
        gl.DeleteVertexArrays(1, &ctx->vao);
    ctx->fullscreen_vbo = ctx->skybox_vbo = 0;
    ctx->depth_probe_fence = NULL;
    ctx->depth_probe_pbo = 0;
    ctx->default_white_tex = 0;
    ctx->brdf_lut_tex = 0;
    ctx->default_white_cubemap = 0;
    ctx->morph_normal_tbo = ctx->morph_tbo = 0;
    ctx->morph_normal_buffer = ctx->bone_ubo = ctx->prev_bone_ubo = 0;
    ctx->morph_buffer = ctx->instance_vbo = ctx->prev_instance_vbo = 0;
    ctx->mesh_vbo = ctx->mesh_ibo = 0;
    ctx->skybox_vao = ctx->fullscreen_vao = ctx->vao = 0;
}

/// @brief Initialize fixed GL render state used before per-draw overrides.
static void gl_init_default_render_state(void) {
    gl.Enable(GL_DEPTH_TEST);
    gl.DepthFunc(GL_LESS);
    gl.Enable(GL_CULL_FACE);
    gl.CullFace(GL_BACK);
    gl.FrontFace(GL_CCW);
    gl.Disable(GL_BLEND);
    gl.BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    gl.PolygonMode(GL_FRONT_AND_BACK, GL_FILL);
}

/// @brief Free CPU-owned context storage when GL object deletion is no longer possible.
///
/// This path is used only during destruction after `glXMakeCurrent` fails. Destroying the GLX
/// context lets the driver reclaim context-owned objects, but host arrays and copied post-FX chain
/// state still need explicit release.
/// @param ctx Borrowed OpenGL context whose CPU allocations and chain ownership are released.
static void gl_release_host_only_context_storage(gl_context_t *ctx) {
    if (!ctx)
        return;
    free(ctx->texture_cache);
    ctx->texture_cache = NULL;
    ctx->texture_cache_count = 0;
    ctx->texture_cache_capacity = 0;
    free(ctx->cubemap_cache);
    ctx->cubemap_cache = NULL;
    ctx->cubemap_cache_count = 0;
    ctx->cubemap_cache_capacity = 0;
    free(ctx->morph_cache);
    ctx->morph_cache = NULL;
    ctx->morph_cache_count = 0;
    ctx->morph_cache_capacity = 0;
    free(ctx->cache_age_scratch);
    ctx->cache_age_scratch = NULL;
    ctx->cache_age_scratch_capacity = 0;
    free(ctx->readback_scratch_rgba);
    ctx->readback_scratch_rgba = NULL;
    ctx->readback_scratch_bytes = 0;
    vgfx3d_postfx_chain_free(&ctx->gpu_postfx_chain);
}

/// @brief Allocate and initialize the entire OpenGL backend context.
///
/// Creates: GLX or Wayland/EGL context (3.3 core profile preferred, fallback to
/// any), shader programs (main, shadow, skybox, postfx), VAOs +
/// VBOs (mesh, instance, fullscreen-triangle, skybox cube), bone
/// UBOs, morph TBOs. Returns NULL on any failure path; caller falls
/// back to the software backend.
///
/// FBConfig selection picks the first config matching the window's
/// X visual to avoid drawable-incompatibility errors at MakeCurrent.
/// @param win Borrowed Zanna window whose native handles and framebuffer are used.
/// @param w Initial logical output width in pixels.
/// @param h Initial logical output height in pixels.
/// @return A fully initialized backend context, or NULL after releasing partial state on failure.
static void *gl_create_ctx(vgfx_window_t win, int32_t w, int32_t h) {
    gl_shader_set_t shaders;
    vgfx_native_handles_t handles;
    if (!vgfx_get_native_handles(win, &handles) ||
        (handles.backend != VGFX_NATIVE_BACKEND_X11 &&
         handles.backend != VGFX_NATIVE_BACKEND_WAYLAND) ||
        load_gl(handles.backend == VGFX_NATIVE_BACKEND_WAYLAND) != 0)
        return NULL;

    gl_context_t *ctx = (gl_context_t *)calloc(1, sizeof(gl_context_t));
    if (!ctx)
        return NULL;
    ctx->vgfx_win = win;
    if (handles.backend == VGFX_NATIVE_BACKEND_WAYLAND) {
        ctx->egl_binding = vgfx3d_egl_wayland_create(handles.display, handles.surface, w, h);
    }
#if !defined(ZANNA_GRAPHICS_WAYLAND)
    else {
        ctx->display = (Display *)handles.display;
        ctx->window = (Window)handles.window;
        int fb_attribs[] = {GLX_RENDER_TYPE,
                            GLX_RGBA_BIT,
                            GLX_DRAWABLE_TYPE,
                            GLX_WINDOW_BIT,
                            GLX_DOUBLEBUFFER,
                            1,
                            GLX_RED_SIZE,
                            8,
                            GLX_GREEN_SIZE,
                            8,
                            GLX_BLUE_SIZE,
                            8,
                            GLX_DEPTH_SIZE,
                            24,
                            0};
        int fb_count = 0;
        GLXFBConfig *configs =
            glx.ChooseFBConfig(ctx->display, DefaultScreen(ctx->display), fb_attribs, &fb_count);
        if (configs && fb_count > 0) {
            GLXFBConfig chosen =
                gl_pick_matching_fb_config(ctx->display, ctx->window, configs, fb_count);
            if (chosen && glx.CreateContextAttribsARB) {
                const int ctx_attribs[] = {GLX_CONTEXT_MAJOR_VERSION_ARB,
                                           3,
                                           GLX_CONTEXT_MINOR_VERSION_ARB,
                                           3,
                                           GLX_CONTEXT_PROFILE_MASK_ARB,
                                           GLX_CONTEXT_CORE_PROFILE_BIT_ARB,
                                           0};
                ctx->glxCtx =
                    glx.CreateContextAttribsARB(ctx->display, chosen, NULL, 1, ctx_attribs);
            }
            if (chosen && !ctx->glxCtx)
                ctx->glxCtx = glx.CreateNewContext(ctx->display, chosen, GLX_RGBA_TYPE, NULL, 1);
            XFree(configs);
        }
        if (ctx->glxCtx)
            XSync(ctx->display, False);
    }
#endif
    if (!gl_make_current(ctx)) {
        gl_destroy_native_binding(ctx);
        free(ctx);
        return NULL;
    }
    if (!gl_query_context_capabilities(ctx)) {
        gl_destroy_native_binding(ctx);
        free(ctx);
        return NULL;
    }
    gl.Enable(GL_TEXTURE_CUBE_MAP_SEAMLESS);
    /* Reversed-Z depth precision: with GL_ZERO_TO_ONE clip control the
     * fixed [-1,1]->[0,1] window remap is dropped, so the reversed depth
     * distribution keeps its float density at the far field. Opt-in via
     * ZANNA_GL_CLIP_CONTROL=1 until validated on a live Linux/GL machine
     * (this TU has no compile or run coverage on the macOS dev host); the
     * shader prologue (compile_shader_parts) and the VS z remap key off
     * ctx->zero_to_one_clip, so flipping the default is a one-line change. */
    ctx->zero_to_one_clip = 0;
    {
        const char *clip_opt = getenv("ZANNA_GL_CLIP_CONTROL");
        if (clip_opt && strcmp(clip_opt, "1") == 0 && gl.ClipControl) {
            gl.ClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE);
            ctx->zero_to_one_clip = 1;
        }
    }
    gl_init_context_defaults(ctx, w, h);
    gl_init_timer_queries(ctx);
    gl_query_default_framebuffer_buffer(ctx);
    gl_probe_default_framebuffer_writable(ctx);
    {
        int present_override = gl_present_override_mode();
        if (ctx->egl_binding) {
            /* An EGL window surface is the native Wayland presentation path. */
            ctx->default_framebuffer_writable = 1;
        } else if (present_override == 0) {
            /* Explicit ZANNA_OPENGL_PRESENT=offscreen: force the compatibility resolve. */
            ctx->default_framebuffer_writable = 0;
        } else if (present_override == 1) {
            /*
             * Trust gl_probe_default_framebuffer_writable. A GLX default drawable can still pass
             * a clear/read probe and then reject the fullscreen shader resolve used by post-FX,
             * so this is not proof on its own — gl_demote_to_offscreen_present() catches that
             * case on the first failed composite and moves the context to the ZannaGFX route for
             * the rest of its life. Presuming failure instead cost every X11/GLX frame a
             * full-screen glReadPixels plus a CPU blit on hardware that presents directly.
             */
        } else if (present_override == 2) {
            ctx->default_framebuffer_writable = 1;
        }
        ctx->present_path = ctx->default_framebuffer_writable ? 1 : 2;
        ctx->stats.present_path = ctx->present_path;
        ctx->stats.default_framebuffer_writable = ctx->default_framebuffer_writable ? 1 : 0;
        if (gl_debug_enabled()) {
            fprintf(stderr,
                    "[OpenGL] present override=%d path=%s default_fb_writable=%d\n",
                    present_override,
                    ctx->present_path == 1 ? "direct" : "offscreen",
                    (int)ctx->default_framebuffer_writable);
        }
    }
    gl_query_anisotropy_support(ctx);

    if (!gl_compile_context_shaders(&shaders, ctx->zero_to_one_clip)) {
        gl_delete_context_shaders(&shaders);
        gl_destroy_native_binding(ctx);
        free(ctx);
        return NULL;
    }

    if (!gl_link_context_programs(ctx, &shaders)) {
        gl_delete_context_programs(ctx);
        gl_delete_context_shaders(&shaders);
        gl_destroy_native_binding(ctx);
        free(ctx);
        return NULL;
    }
    gl_delete_context_shaders(&shaders);

    query_main_uniforms(ctx);
    if (!gl_query_unlit_uniforms(ctx)) {
        gl_delete_context_programs(ctx);
        gl_destroy_native_binding(ctx);
        free(ctx);
        return NULL;
    }
    query_shadow_uniforms(ctx);
    query_skybox_uniforms(ctx);
    query_postfx_uniforms(ctx);
    query_bloom_taa_uniforms(ctx);

    gl_bind_default_sampler_uniforms(ctx);
    {
        GLint full_locations[GL_MAIN_UNIFORM_SNAPSHOT_CAPACITY];
        GLuint full_program = ctx->program;
        gl_save_main_uniform_locations(ctx, full_locations);
        gl_load_main_uniform_locations(ctx, ctx->unlit_uniform_locations);
        ctx->program = ctx->unlit_program;
        gl_bind_main_sampler_uniforms(ctx);
        ctx->program = full_program;
        gl_load_main_uniform_locations(ctx, full_locations);
    }
    gl_bind_context_uniform_blocks(ctx);
    if (!gl_generate_context_objects(ctx) || !gl_init_streaming_buffers(ctx) ||
        !gl_init_static_geometry_buffers(ctx) || !gl_create_default_white_texture(ctx) ||
        !gl_create_brdf_lut_texture(ctx) || !gl_create_default_white_cubemap(ctx)) {
        gl_release_fixed_context_objects(ctx);
        gl_delete_context_programs(ctx);
        gl_destroy_native_binding(ctx);
        free(ctx);
        return NULL;
    }
    gl_init_default_render_state();

    return ctx;
}

/// @brief Tear down the GL backend context — releases everything from `_create_ctx`.
///
/// Order matters: caches first (GL objects depend on the context),
/// then offscreen targets, then the master VAOs/VBOs/UBOs/programs,
/// finally the GLX context itself.
/// @param ctx_ptr Owned OpenGL backend context to destroy; may be NULL.
static void gl_destroy_ctx(void *ctx_ptr) {
    gl_context_t *ctx = (gl_context_t *)ctx_ptr;
    if (!ctx)
        return;

    if (!gl_make_current(ctx)) {
        gl_release_host_only_context_storage(ctx);
        gl_destroy_native_binding(ctx);
        free(ctx);
        return;
    }

    gl_destroy_timer_queries(ctx);
    texture_cache_destroy(ctx);
    gl_destroy_mesh_cache(ctx);
    destroy_scene_targets(ctx);
    destroy_postfx_readback_target(ctx);
    destroy_postfx_scratch_target(ctx);
    if (ctx->postfx_lut_tex) {
        gl.DeleteTextures(1, &ctx->postfx_lut_tex);
        ctx->postfx_lut_tex = 0;
    }
    destroy_present_target(ctx);
    destroy_bloom_targets(ctx);
    destroy_taa_targets(ctx);
    destroy_ssr_target(ctx);
    destroy_rtt_targets(ctx);
    destroy_shadow_targets(ctx);
    vgfx3d_postfx_chain_free(&ctx->gpu_postfx_chain);

    for (int ws = 0; ws < 3; ws++) {
        for (int wt = 0; wt < 3; wt++) {
            for (int filter = 0; filter < 12; filter++) {
                for (int anisotropy = 0; anisotropy < VGFX3D_OPENGL_ANISOTROPY_LEVEL_COUNT;
                     anisotropy++) {
                    if (ctx->material_samplers[ws][wt][filter][anisotropy])
                        gl.DeleteSamplers(1, &ctx->material_samplers[ws][wt][filter][anisotropy]);
                }
            }
        }
    }

    gl_release_fixed_context_objects(ctx);
    gl_delete_context_programs(ctx);
    free(ctx->readback_scratch_rgba);
    ctx->readback_scratch_rgba = NULL;
    ctx->readback_scratch_bytes = 0;
    gl_destroy_native_binding(ctx);
    free(ctx);
}
