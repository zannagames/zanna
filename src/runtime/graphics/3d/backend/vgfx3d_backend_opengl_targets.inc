//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/backend/vgfx3d_backend_opengl_targets.inc
// Purpose: OpenGL target lifecycle, draw-output routing, readback, and post-FX application.
// Key invariants:
//   - FBO completeness and exact pixel layouts are checked before a target becomes active.
//   - Failed readback leaves a CPU render-target mirror dirty rather than publishing stale bytes.
// Ownership/Lifetime:
//   - GL names belong to gl_context_t and are deleted by the matching target teardown helper.
//   - Runtime render-target shells are borrowed only while their Canvas owner retains them.
// Links: vgfx3d_backend_opengl.c, vgfx3d_backend_opengl_frame.inc
//
//===----------------------------------------------------------------------===//

/**
 * @file vgfx3d_backend_opengl_targets.inc
 * @brief Implements OpenGL framebuffer targets, post-processing passes, and render-target
 *        readback for the 3D runtime backend.
 *
 * This implementation fragment owns the size-dependent OpenGL objects used for scene
 * rendering, presentation, bloom, temporal anti-aliasing, screen-space reflections,
 * user render targets, and shadow maps. It also routes draw calls to the active framebuffer
 * and preserves/restores OpenGL state around full-screen post-processing work.
 */

static void bind_texture_unit(GLint uniform_loc, int unit, GLenum target, GLuint texture);

/// @brief Validate a 2D allocation extent against the live OpenGL device limit.
/// @param ctx Borrowed initialized context containing GL_MAX_TEXTURE_SIZE.
/// @param width Requested positive texture width.
/// @param height Requested positive texture height.
/// @return Non-zero only when both dimensions are representable by the current device.
static int gl_target_extent_is_valid(const gl_context_t *ctx, int32_t width, int32_t height) {
    return ctx && width > 0 && height > 0 && ctx->max_texture_size > 0 &&
           width <= ctx->max_texture_size && height <= ctx->max_texture_size;
}

typedef struct {
    GLuint fbo;
    GLuint color_tex;
    GLuint motion_tex;
    GLuint postfx_tex;
    GLuint depth_tex;
} gl_scene_target_set_t;

/// @brief Delete every name in a detached scene-target candidate.
/// @param set Mutable caller-owned name set, zeroed after release.
static void gl_release_scene_target_set(gl_scene_target_set_t *set) {
    if (!set)
        return;
    if (set->depth_tex)
        gl.DeleteTextures(1, &set->depth_tex);
    if (set->postfx_tex)
        gl.DeleteTextures(1, &set->postfx_tex);
    if (set->motion_tex)
        gl.DeleteTextures(1, &set->motion_tex);
    if (set->color_tex)
        gl.DeleteTextures(1, &set->color_tex);
    if (set->fbo)
        gl.DeleteFramebuffers(1, &set->fbo);
    memset(set, 0, sizeof(*set));
}

/// @brief Allocate and validate one detached scene-target set.
/// @param ctx Borrowed context providing depth-format capabilities.
/// @param width Validated texture width.
/// @param height Validated texture height.
/// @param hdr Non-zero to allocate float scene and post-effect color attachments.
/// @param out_set Receives the complete candidate on success and remains empty on failure.
/// @return Non-zero when every name exists and the framebuffer is complete.
static int gl_build_scene_target_set(
    gl_context_t *ctx, int32_t width, int32_t height, int hdr, gl_scene_target_set_t *out_set) {
    gl_scene_target_set_t candidate = {0};
    GLenum framebuffer_status;
    int allocation_ok;
    const GLint color_format = hdr ? GL_RGBA16F : GL_RGBA8;
    const GLenum color_type = hdr ? GL_FLOAT : GL_UNSIGNED_BYTE;
    const GLint depth_format =
        ctx->supports_depth_float_target ? GL_DEPTH_COMPONENT32F : GL_DEPTH_COMPONENT24;
    const GLenum depth_type = ctx->supports_depth_float_target ? GL_FLOAT : GL_UNSIGNED_INT;

    if (!out_set)
        return 0;
    memset(out_set, 0, sizeof(*out_set));
    gl.GenFramebuffers(1, &candidate.fbo);
    gl.BindFramebuffer(GL_FRAMEBUFFER, candidate.fbo);

    gl.GenTextures(1, &candidate.color_tex);
    gl.BindTexture(GL_TEXTURE_2D, candidate.color_tex);
    gl.TexImage2D(GL_TEXTURE_2D, 0, color_format, width, height, 0, GL_RGBA, color_type, NULL);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl.FramebufferTexture2D(
        GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, candidate.color_tex, 0);

    gl.GenTextures(1, &candidate.motion_tex);
    gl.BindTexture(GL_TEXTURE_2D, candidate.motion_tex);
    gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl.FramebufferTexture2D(
        GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, candidate.motion_tex, 0);

    gl.GenTextures(1, &candidate.postfx_tex);
    gl.BindTexture(GL_TEXTURE_2D, candidate.postfx_tex);
    gl.TexImage2D(GL_TEXTURE_2D, 0, color_format, width, height, 0, GL_RGBA, color_type, NULL);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl.FramebufferTexture2D(
        GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT2, GL_TEXTURE_2D, candidate.postfx_tex, 0);

    gl.GenTextures(1, &candidate.depth_tex);
    gl.BindTexture(GL_TEXTURE_2D, candidate.depth_tex);
    gl.TexImage2D(
        GL_TEXTURE_2D, 0, depth_format, width, height, 0, GL_DEPTH_COMPONENT, depth_type, NULL);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    gl.FramebufferTexture2D(
        GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, candidate.depth_tex, 0);
    {
        const GLenum attachments[2] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
        gl.DrawBuffers(2, attachments);
    }
    gl.ReadBuffer(GL_COLOR_ATTACHMENT0);

    /* Always consume allocation errors before a possible HDR-to-LDR retry. Short-circuiting on
     * an incomplete framebuffer would otherwise leave the HDR error queued, making the valid LDR
     * candidate fail its later error check. */
    framebuffer_status = gl.CheckFramebufferStatus(GL_FRAMEBUFFER);
    allocation_ok = gl_upload_ok();
    if (!candidate.fbo || !candidate.color_tex || !candidate.motion_tex || !candidate.postfx_tex ||
        !candidate.depth_tex || framebuffer_status != GL_FRAMEBUFFER_COMPLETE || !allocation_ok) {
        gl_release_scene_target_set(&candidate);
        return 0;
    }
    *out_set = candidate;
    return 1;
}

/// @brief Transactionally create or resize one color-texture framebuffer.
/// @details Candidate names are fully allocated and validated before the previous live pair is
///          released. Framebuffer, viewport, pixel-store, texture, and renderbuffer bindings are
///          restored on every path, with bindings to the replaced pair redirected to the new pair.
/// @param ctx Borrowed initialized OpenGL context.
/// @param width Requested target width.
/// @param height Requested target height.
/// @param hdr Non-zero for RGBA16F, zero for RGBA8.
/// @param io_fbo Mutable owner of the live framebuffer name.
/// @param io_texture Mutable owner of the live texture name.
/// @param io_width Mutable stored width.
/// @param io_height Mutable stored height.
/// @param io_hdr Optional mutable stored precision discriminator.
/// @return Zero when a matching complete target is live, otherwise -1 with the old target intact.
static int gl_ensure_color_target(gl_context_t *ctx,
                                  int32_t width,
                                  int32_t height,
                                  int hdr,
                                  GLuint *io_fbo,
                                  GLuint *io_texture,
                                  int32_t *io_width,
                                  int32_t *io_height,
                                  int8_t *io_hdr) {
    gl_framebuffer_state_t saved;
    GLuint candidate_fbo = 0;
    GLuint candidate_texture = 0;
    GLuint previous_fbo;
    GLuint previous_texture;
    const GLint internal_format = hdr ? GL_RGBA16F : GL_RGBA8;
    const GLenum data_type = hdr ? GL_FLOAT : GL_UNSIGNED_BYTE;

    if (!gl_target_extent_is_valid(ctx, width, height) || !io_fbo || !io_texture || !io_width ||
        !io_height) {
        return -1;
    }
    if (*io_fbo && *io_texture && *io_width == width && *io_height == height &&
        (!io_hdr || *io_hdr == (int8_t)(hdr != 0))) {
        return 0;
    }

    gl_capture_framebuffer_state(&saved);
    (void)gl_drain_errors("pre-color-target-allocation");
    gl.GenFramebuffers(1, &candidate_fbo);
    gl.BindFramebuffer(GL_FRAMEBUFFER, candidate_fbo);
    gl.GenTextures(1, &candidate_texture);
    gl.BindTexture(GL_TEXTURE_2D, candidate_texture);
    gl.TexImage2D(GL_TEXTURE_2D, 0, internal_format, width, height, 0, GL_RGBA, data_type, NULL);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl.FramebufferTexture2D(
        GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, candidate_texture, 0);
    gl.DrawBuffer(GL_COLOR_ATTACHMENT0);
    gl.ReadBuffer(GL_COLOR_ATTACHMENT0);
    if (!candidate_fbo || !candidate_texture ||
        gl.CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE || !gl_upload_ok()) {
        if (candidate_texture)
            gl.DeleteTextures(1, &candidate_texture);
        if (candidate_fbo)
            gl.DeleteFramebuffers(1, &candidate_fbo);
        gl_restore_framebuffer_state(&saved);
        return -1;
    }

    previous_fbo = *io_fbo;
    previous_texture = *io_texture;
    /* Same aliasing hazard as gl_build_scene_target_set's caller: name zero
     * denotes the default framebuffer or "no texture", never this target, so a
     * first allocation must not capture a caller's default-framebuffer binding
     * and replay its draw buffer against the new FBO. */
    if (previous_fbo != 0) {
        if ((GLuint)saved.draw_framebuffer == previous_fbo)
            saved.draw_framebuffer = (GLint)candidate_fbo;
        if ((GLuint)saved.read_framebuffer == previous_fbo)
            saved.read_framebuffer = (GLint)candidate_fbo;
    }
    if (previous_texture != 0 && (GLuint)saved.texture_binding_2d == previous_texture)
        saved.texture_binding_2d = (GLint)candidate_texture;
    *io_fbo = candidate_fbo;
    *io_texture = candidate_texture;
    *io_width = width;
    *io_height = height;
    if (io_hdr)
        *io_hdr = (int8_t)(hdr != 0);
    if (previous_texture)
        gl.DeleteTextures(1, &previous_texture);
    if (previous_fbo)
        gl.DeleteFramebuffers(1, &previous_fbo);
    gl_restore_framebuffer_state(&saved);
    return 0;
}

/// @brief Release the scene FBO and its color/motion/depth attachments.
/// @param ctx Borrowed OpenGL context whose scene targets are released; null is a no-op.
static void destroy_scene_targets(gl_context_t *ctx) {
    if (!ctx)
        return;
    if (ctx->scene_depth_tex)
        gl.DeleteTextures(1, &ctx->scene_depth_tex);
    if (ctx->scene_motion_tex)
        gl.DeleteTextures(1, &ctx->scene_motion_tex);
    if (ctx->scene_postfx_tex)
        gl.DeleteTextures(1, &ctx->scene_postfx_tex);
    if (ctx->scene_color_tex)
        gl.DeleteTextures(1, &ctx->scene_color_tex);
    if (ctx->scene_fbo)
        gl.DeleteFramebuffers(1, &ctx->scene_fbo);
    ctx->scene_fbo = 0;
    ctx->scene_color_tex = 0;
    ctx->scene_motion_tex = 0;
    ctx->scene_postfx_tex = 0;
    ctx->scene_depth_tex = 0;
    ctx->scene_width = 0;
    ctx->scene_height = 0;
    ctx->scene_hdr_active = 0;
}

/// @brief Release the post-FX readback FBO + texture.
///
/// The readback target stages tonemapped post-FX output before
/// `glReadPixels` copies to host memory for screenshot capture.
/// @param ctx Borrowed OpenGL context whose readback target is released; null is a no-op.
static void destroy_postfx_readback_target(gl_context_t *ctx) {
    if (!ctx)
        return;
    if (ctx->postfx_readback_tex)
        gl.DeleteTextures(1, &ctx->postfx_readback_tex);
    if (ctx->postfx_readback_fbo)
        gl.DeleteFramebuffers(1, &ctx->postfx_readback_fbo);
    ctx->postfx_readback_tex = 0;
    ctx->postfx_readback_fbo = 0;
    ctx->postfx_readback_width = 0;
    ctx->postfx_readback_height = 0;
    ctx->postfx_readback_hdr = 0;
}

/// @brief Build a scene FBO with color (HDR/LDR), motion-vector, and depth attachments.
///
/// MRT layout (color attachment 0 = scene color, 1 = motion vectors).
/// Color format chosen by the active context capabilities — HDR16F when the
/// driver can render to float color targets, RGBA8 otherwise.
/// Depth uses the 32-bit float format so post-FX can reconstruct
/// world position with high precision.
/// Verifies framebuffer completeness before returning success.
/// @param ctx Borrowed initialized OpenGL context that owns the resulting objects.
/// @param w Positive scene width in pixels.
/// @param h Positive scene height in pixels.
/// @return 0 when a complete scene framebuffer of the requested size is ready, otherwise -1.
static int ensure_scene_targets(gl_context_t *ctx, int32_t w, int32_t h) {
    gl_framebuffer_state_t saved;
    gl_scene_target_set_t candidate = {0};
    gl_scene_target_set_t previous;
    int want_hdr;

    if (!gl_target_extent_is_valid(ctx, w, h))
        return -1;
    if (ctx->scene_fbo && ctx->scene_color_tex && ctx->scene_motion_tex && ctx->scene_postfx_tex &&
        ctx->scene_depth_tex && ctx->scene_width == w && ctx->scene_height == h) {
        return 0;
    }

    /* Plan 05: the window scene target follows the shared HDR policy (RGBA16F) so bloom
     * and tonemapping operate on unclamped linear color; ancient drivers without float
     * color targets fall back to the legacy RGBA8 path (scene_hdr_active records which). */
    want_hdr = (vgfx3d_opengl_choose_color_format(VGFX3D_OPENGL_TARGET_SCENE) ==
                    VGFX3D_OPENGL_COLOR_FORMAT_HDR16F &&
                ctx->supports_hdr_color_target)
                   ? 1
                   : 0;

    gl_capture_framebuffer_state(&saved);
    (void)gl_drain_errors("pre-scene-target-allocation");
    for (;;) {
        if (gl_build_scene_target_set(ctx, w, h, want_hdr, &candidate))
            break;
        if (want_hdr) {
            want_hdr = 0; /* retry once with the legacy LDR layout */
            continue;
        }
        gl_restore_framebuffer_state(&saved);
        return -1;
    }

    previous.fbo = ctx->scene_fbo;
    previous.color_tex = ctx->scene_color_tex;
    previous.motion_tex = ctx->scene_motion_tex;
    previous.postfx_tex = ctx->scene_postfx_tex;
    previous.depth_tex = ctx->scene_depth_tex;
    /* Redirect only bindings that actually named the set being replaced. Name
     * zero is not one of this set's objects: as a framebuffer it is the default
     * framebuffer and as a texture it means "none". On the first allocation
     * every `previous` name is zero, so an unguarded comparison matches a
     * caller that had the default framebuffer bound and rewrites the restore to
     * bind the new FBO while still replaying the default framebuffer's
     * GL_BACK draw buffer — which is invalid on a non-default framebuffer and
     * leaves GL_INVALID_OPERATION queued for whatever runs next. */
    if (previous.fbo != 0) {
        if ((GLuint)saved.draw_framebuffer == previous.fbo)
            saved.draw_framebuffer = (GLint)candidate.fbo;
        if ((GLuint)saved.read_framebuffer == previous.fbo)
            saved.read_framebuffer = (GLint)candidate.fbo;
    }
    if ((GLuint)saved.texture_binding_2d != 0) {
        if ((GLuint)saved.texture_binding_2d == previous.color_tex)
            saved.texture_binding_2d = (GLint)candidate.color_tex;
        else if ((GLuint)saved.texture_binding_2d == previous.motion_tex)
            saved.texture_binding_2d = (GLint)candidate.motion_tex;
        else if ((GLuint)saved.texture_binding_2d == previous.postfx_tex)
            saved.texture_binding_2d = (GLint)candidate.postfx_tex;
        else if ((GLuint)saved.texture_binding_2d == previous.depth_tex)
            saved.texture_binding_2d = (GLint)candidate.depth_tex;
    }
    ctx->scene_fbo = candidate.fbo;
    ctx->scene_color_tex = candidate.color_tex;
    ctx->scene_motion_tex = candidate.motion_tex;
    ctx->scene_postfx_tex = candidate.postfx_tex;
    ctx->scene_depth_tex = candidate.depth_tex;
    memset(&candidate, 0, sizeof(candidate));
    ctx->scene_hdr_active = (int8_t)want_hdr;
    /* Scene target recreation orphans any TAA history sized for the old target. */
    ctx->taa_history_valid = 0;
    ctx->scene_width = w;
    ctx->scene_height = h;
    gl_release_scene_target_set(&previous);
    gl_restore_framebuffer_state(&saved);
    return 0;
}

/// @brief Build a single-attachment FBO for tonemapped post-FX readback.
/// @details Reuses an existing target at the requested size and otherwise recreates it using
///          the active scene precision so intermediate HDR values remain representable.
/// @param ctx Borrowed initialized OpenGL context that owns the resulting objects.
/// @param w Positive readback width in pixels.
/// @param h Positive readback height in pixels.
/// @return 0 when a complete readback target is ready, otherwise -1 after partial cleanup.
static int ensure_postfx_readback_target(gl_context_t *ctx, int32_t w, int32_t h) {
    return gl_ensure_color_target(ctx,
                                  w,
                                  h,
                                  ctx ? ctx->scene_hdr_active : 0,
                                  ctx ? &ctx->postfx_readback_fbo : NULL,
                                  ctx ? &ctx->postfx_readback_tex : NULL,
                                  ctx ? &ctx->postfx_readback_width : NULL,
                                  ctx ? &ctx->postfx_readback_height : NULL,
                                  ctx ? &ctx->postfx_readback_hdr : NULL);
}

/// @brief Release the intermediate post-FX scratch FBO + texture.
///
/// The scratch target forms the second side of the post-FX ping-pong pair. Keeping it separate
/// from the scene FBO preserves the original scene color while multi-pass effects run.
/// @param ctx Borrowed OpenGL context whose scratch target is released; null is a no-op.
static void destroy_postfx_scratch_target(gl_context_t *ctx) {
    if (!ctx)
        return;
    if (ctx->postfx_scratch_tex)
        gl.DeleteTextures(1, &ctx->postfx_scratch_tex);
    if (ctx->postfx_scratch_fbo)
        gl.DeleteFramebuffers(1, &ctx->postfx_scratch_fbo);
    ctx->postfx_scratch_tex = 0;
    ctx->postfx_scratch_fbo = 0;
    ctx->postfx_scratch_width = 0;
    ctx->postfx_scratch_height = 0;
    ctx->postfx_scratch_hdr = 0;
}

/// @brief Build a single-attachment LDR FBO used as post-FX intermediate storage.
/// @param ctx Borrowed initialized OpenGL context that owns the resulting objects.
/// @param w Positive scratch-target width in pixels.
/// @param h Positive scratch-target height in pixels.
/// @return Zero on success, -1 when allocation or framebuffer completeness fails.
static int ensure_postfx_scratch_target(gl_context_t *ctx, int32_t w, int32_t h) {
    return gl_ensure_color_target(ctx,
                                  w,
                                  h,
                                  ctx ? ctx->scene_hdr_active : 0,
                                  ctx ? &ctx->postfx_scratch_fbo : NULL,
                                  ctx ? &ctx->postfx_scratch_tex : NULL,
                                  ctx ? &ctx->postfx_scratch_width : NULL,
                                  ctx ? &ctx->postfx_scratch_height : NULL,
                                  ctx ? &ctx->postfx_scratch_hdr : NULL);
}

/// @brief Release the logical-output-sized OpenGL presentation FBO and color texture.
/// @details This target is separate from scene-sized post-FX ping-pong storage so a reduced scene
///          can be sampled into a native-size final image without invalidating an intermediate
///          that is still the source of the last pass.
/// @param ctx Borrowed OpenGL context that owns the objects; null is a no-op.
static void destroy_present_target(gl_context_t *ctx) {
    if (!ctx)
        return;
    if (ctx->present_tex)
        gl.DeleteTextures(1, &ctx->present_tex);
    if (ctx->present_fbo)
        gl.DeleteFramebuffers(1, &ctx->present_fbo);
    ctx->present_tex = 0;
    ctx->present_fbo = 0;
    ctx->present_width = 0;
    ctx->present_height = 0;
}

/// @brief Allocate an RGBA8 final-composite target at the logical output extent.
/// @details The target is used for screenshot readback and for the ZannaGFX presentation fallback
///          when the GLX default framebuffer is not writable. Linear filtering in the full-screen
///          pass provides the explicit scene upscale; the texture itself is never scene-sized.
/// @param ctx Borrowed initialized OpenGL context.
/// @param w Positive logical output width.
/// @param h Positive logical output height.
/// @return 0 when a complete target of exactly @p w × @p h is ready, otherwise -1 after cleaning
///         any partial objects.
static int ensure_present_target(gl_context_t *ctx, int32_t w, int32_t h) {
    return gl_ensure_color_target(ctx,
                                  w,
                                  h,
                                  0,
                                  ctx ? &ctx->present_fbo : NULL,
                                  ctx ? &ctx->present_tex : NULL,
                                  ctx ? &ctx->present_width : NULL,
                                  ctx ? &ctx->present_height : NULL,
                                  NULL);
}

/// @brief Release the Plan 05 bloom mip-chain textures and FBOs.
/// @param ctx Borrowed OpenGL context whose bloom targets are released; null is a no-op.
static void destroy_bloom_targets(gl_context_t *ctx) {
    if (!ctx)
        return;
    for (int32_t i = 0; i < 6; i++) {
        if (ctx->bloom_mip_tex[i])
            gl.DeleteTextures(1, &ctx->bloom_mip_tex[i]);
        if (ctx->bloom_mip_fbo[i])
            gl.DeleteFramebuffers(1, &ctx->bloom_mip_fbo[i]);
        ctx->bloom_mip_tex[i] = 0;
        ctx->bloom_mip_fbo[i] = 0;
        ctx->bloom_mip_w[i] = 0;
        ctx->bloom_mip_h[i] = 0;
    }
    ctx->bloom_mip_count = 0;
    ctx->bloom_base_width = 0;
    ctx->bloom_base_height = 0;
    ctx->bloom_hdr = 0;
}

/// @brief Build the Plan 05 half-res RGBA16F bloom mip chain for a scene of (w, h).
/// @details Levels halve until the next level would drop below 8px on either axis
///          (at most 6 levels). Rebuilt only when the scene size changes.
/// @param ctx Borrowed initialized OpenGL context that owns the mip-chain objects.
/// @param w Positive full-resolution scene width used to derive the first half-resolution mip.
/// @param h Positive full-resolution scene height used to derive the first half-resolution mip.
/// @return Zero on success, -1 when allocation or completeness fails.
static int ensure_bloom_targets(gl_context_t *ctx, int32_t w, int32_t h) {
    gl_framebuffer_state_t saved;
    GLuint candidate_tex[6] = {0};
    GLuint candidate_fbo[6] = {0};
    int32_t candidate_w[6] = {0};
    int32_t candidate_h[6] = {0};
    GLuint previous_tex[6];
    GLuint previous_fbo[6];
    int32_t candidate_count = 0;
    const int hdr = ctx ? ctx->scene_hdr_active : 0;
    const GLint internal_format = hdr ? GL_RGBA16F : GL_RGBA8;
    const GLenum data_type = hdr ? GL_FLOAT : GL_UNSIGNED_BYTE;
    int32_t mw;
    int32_t mh;

    if (!gl_target_extent_is_valid(ctx, w, h) || !ctx->bloom_down_program ||
        !ctx->bloom_up_program) {
        return -1;
    }
    if (ctx->bloom_mip_count > 0 && ctx->bloom_base_width == w && ctx->bloom_base_height == h &&
        ctx->bloom_hdr == (int8_t)hdr) {
        int complete = 1;
        for (int32_t i = 0; i < ctx->bloom_mip_count; i++) {
            if (!ctx->bloom_mip_tex[i] || !ctx->bloom_mip_fbo[i]) {
                complete = 0;
                break;
            }
        }
        if (complete)
            return 0;
    }

    gl_capture_framebuffer_state(&saved);
    (void)gl_drain_errors("pre-bloom-target-allocation");
    mw = w / 2;
    mh = h / 2;
    for (int32_t i = 0; i < 6 && mw >= 1 && mh >= 1; i++) {
        gl.GenFramebuffers(1, &candidate_fbo[i]);
        gl.BindFramebuffer(GL_FRAMEBUFFER, candidate_fbo[i]);
        gl.GenTextures(1, &candidate_tex[i]);
        gl.BindTexture(GL_TEXTURE_2D, candidate_tex[i]);
        gl.TexImage2D(GL_TEXTURE_2D, 0, internal_format, mw, mh, 0, GL_RGBA, data_type, NULL);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        gl.FramebufferTexture2D(
            GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, candidate_tex[i], 0);
        gl.DrawBuffer(GL_COLOR_ATTACHMENT0);
        if (!candidate_fbo[i] || !candidate_tex[i] ||
            gl.CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE ||
            !gl_upload_ok()) {
            for (int32_t release = 0; release < 6; release++) {
                if (candidate_tex[release])
                    gl.DeleteTextures(1, &candidate_tex[release]);
                if (candidate_fbo[release])
                    gl.DeleteFramebuffers(1, &candidate_fbo[release]);
            }
            gl_restore_framebuffer_state(&saved);
            return -1;
        }
        candidate_w[i] = mw;
        candidate_h[i] = mh;
        candidate_count = i + 1;
        if (mw / 2 < 8 || mh / 2 < 8)
            break;
        mw /= 2;
        mh /= 2;
    }
    if (candidate_count <= 0) {
        gl_restore_framebuffer_state(&saved);
        return -1;
    }
    memcpy(previous_tex, ctx->bloom_mip_tex, sizeof(previous_tex));
    memcpy(previous_fbo, ctx->bloom_mip_fbo, sizeof(previous_fbo));
    for (int32_t i = 0; i < 6; i++) {
        if ((GLuint)saved.draw_framebuffer == previous_fbo[i])
            saved.draw_framebuffer = (GLint)candidate_fbo[i];
        if ((GLuint)saved.read_framebuffer == previous_fbo[i])
            saved.read_framebuffer = (GLint)candidate_fbo[i];
        if ((GLuint)saved.texture_binding_2d == previous_tex[i])
            saved.texture_binding_2d = (GLint)candidate_tex[i];
        ctx->bloom_mip_tex[i] = candidate_tex[i];
        ctx->bloom_mip_fbo[i] = candidate_fbo[i];
        ctx->bloom_mip_w[i] = candidate_w[i];
        ctx->bloom_mip_h[i] = candidate_h[i];
        if (previous_tex[i])
            gl.DeleteTextures(1, &previous_tex[i]);
        if (previous_fbo[i])
            gl.DeleteFramebuffers(1, &previous_fbo[i]);
    }
    ctx->bloom_mip_count = candidate_count;
    ctx->bloom_base_width = w;
    ctx->bloom_base_height = h;
    ctx->bloom_hdr = (int8_t)hdr;
    gl_restore_framebuffer_state(&saved);
    return 0;
}

/// @brief Release the Plan 05 TAA ping-pong history textures and FBOs.
/// @param ctx Borrowed OpenGL context whose temporal history is released; null is a no-op.
static void destroy_taa_targets(gl_context_t *ctx) {
    if (!ctx)
        return;
    for (int32_t i = 0; i < 2; i++) {
        if (ctx->taa_history_tex[i])
            gl.DeleteTextures(1, &ctx->taa_history_tex[i]);
        if (ctx->taa_history_fbo[i])
            gl.DeleteFramebuffers(1, &ctx->taa_history_fbo[i]);
        ctx->taa_history_tex[i] = 0;
        ctx->taa_history_fbo[i] = 0;
    }
    ctx->taa_history_width = 0;
    ctx->taa_history_height = 0;
    ctx->taa_history_parity = 0;
    ctx->taa_history_valid = 0;
    ctx->taa_history_hdr = 0;
}

/// @brief Build the Plan 05 TAA ping-pong history pair at the active scene precision.
/// @param ctx Borrowed initialized OpenGL context that owns the history objects.
/// @param w Positive scene width in pixels.
/// @param h Positive scene height in pixels.
/// @return Zero on success, -1 when allocation or completeness fails.
static int ensure_taa_targets(gl_context_t *ctx, int32_t w, int32_t h) {
    gl_framebuffer_state_t saved;
    GLuint candidate_tex[2] = {0, 0};
    GLuint candidate_fbo[2] = {0, 0};
    GLuint previous_tex[2];
    GLuint previous_fbo[2];
    const int hdr = ctx ? ctx->scene_hdr_active : 0;
    const GLint internal_format = hdr ? GL_RGBA16F : GL_RGBA8;
    const GLenum data_type = hdr ? GL_FLOAT : GL_UNSIGNED_BYTE;

    if (!gl_target_extent_is_valid(ctx, w, h) || !ctx->taa_program)
        return -1;
    if (ctx->taa_history_fbo[0] && ctx->taa_history_fbo[1] && ctx->taa_history_tex[0] &&
        ctx->taa_history_tex[1] && ctx->taa_history_width == w && ctx->taa_history_height == h &&
        ctx->taa_history_hdr == (int8_t)hdr) {
        return 0;
    }

    gl_capture_framebuffer_state(&saved);
    (void)gl_drain_errors("pre-taa-target-allocation");
    for (int32_t i = 0; i < 2; i++) {
        gl.GenFramebuffers(1, &candidate_fbo[i]);
        gl.BindFramebuffer(GL_FRAMEBUFFER, candidate_fbo[i]);
        gl.GenTextures(1, &candidate_tex[i]);
        gl.BindTexture(GL_TEXTURE_2D, candidate_tex[i]);
        gl.TexImage2D(GL_TEXTURE_2D, 0, internal_format, w, h, 0, GL_RGBA, data_type, NULL);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        gl.FramebufferTexture2D(
            GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, candidate_tex[i], 0);
        gl.DrawBuffer(GL_COLOR_ATTACHMENT0);
        if (!candidate_fbo[i] || !candidate_tex[i] ||
            gl.CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE ||
            !gl_upload_ok()) {
            for (int32_t release = 0; release < 2; release++) {
                if (candidate_tex[release])
                    gl.DeleteTextures(1, &candidate_tex[release]);
                if (candidate_fbo[release])
                    gl.DeleteFramebuffers(1, &candidate_fbo[release]);
            }
            gl_restore_framebuffer_state(&saved);
            return -1;
        }
    }
    memcpy(previous_tex, ctx->taa_history_tex, sizeof(previous_tex));
    memcpy(previous_fbo, ctx->taa_history_fbo, sizeof(previous_fbo));
    for (int32_t i = 0; i < 2; i++) {
        if ((GLuint)saved.draw_framebuffer == previous_fbo[i])
            saved.draw_framebuffer = (GLint)candidate_fbo[i];
        if ((GLuint)saved.read_framebuffer == previous_fbo[i])
            saved.read_framebuffer = (GLint)candidate_fbo[i];
        if ((GLuint)saved.texture_binding_2d == previous_tex[i])
            saved.texture_binding_2d = (GLint)candidate_tex[i];
        ctx->taa_history_tex[i] = candidate_tex[i];
        ctx->taa_history_fbo[i] = candidate_fbo[i];
        if (previous_tex[i])
            gl.DeleteTextures(1, &previous_tex[i]);
        if (previous_fbo[i])
            gl.DeleteFramebuffers(1, &previous_fbo[i]);
    }
    ctx->taa_history_width = w;
    ctx->taa_history_height = h;
    ctx->taa_history_parity = 0;
    ctx->taa_history_valid = 0;
    ctx->taa_history_hdr = (int8_t)hdr;
    gl_restore_framebuffer_state(&saved);
    return 0;
}

/// @brief Draw one fullscreen bloom pass (down- or upsample) into a mip FBO.
/// @param ctx Borrowed OpenGL context whose full-screen vertex state is already active.
/// @param program Linked bloom shader program to execute.
/// @param src_tex Texture containing the source mip.
/// @param src_w Positive source-mip width used to compute inverse texel size.
/// @param src_h Positive source-mip height used to compute inverse texel size.
/// @param dst_fbo Destination framebuffer for the pass.
/// @param dst_w Destination viewport width in pixels.
/// @param dst_h Destination viewport height in pixels.
/// @param uSrcInvSize Uniform location receiving the inverse source dimensions.
/// @param uSrcTexLoc Sampler uniform location bound to texture unit zero.
static void gl_bloom_pass_draw(gl_context_t *ctx,
                               GLuint program,
                               GLuint src_tex,
                               int32_t src_w,
                               int32_t src_h,
                               GLuint dst_fbo,
                               int32_t dst_w,
                               int32_t dst_h,
                               GLint uSrcInvSize,
                               GLint uSrcTexLoc) {
    gl.BindFramebuffer(GL_FRAMEBUFFER, dst_fbo);
    gl.DrawBuffer(GL_COLOR_ATTACHMENT0);
    gl.Viewport(0, 0, dst_w, dst_h);
    gl.UseProgram(program);
    bind_texture_unit(uSrcTexLoc, 0, GL_TEXTURE_2D, src_tex);
    gl.Uniform2f(uSrcInvSize,
                 src_w > 0 ? 1.0f / (float)src_w : 0.0f,
                 src_h > 0 ? 1.0f / (float)src_h : 0.0f);
    gl.DrawArrays(GL_TRIANGLES, 0, 3);
}

/// @brief Encode the Plan 05 bloom mip chain from the scene color texture.
/// @details Pass 0 thresholds + Karis-downsamples the scene into the half-res mip; each
///          further mip is a plain 13-tap downsample. The upsample walk tent-filters each
///          deeper mip and accumulates it into the one above via GL_ONE/GL_ONE blending,
///          leaving mip 0 with the summed octaves.
/// @param ctx Borrowed initialized OpenGL context containing the scene and bloom resources.
/// @param source_tex Current ordered-chain color source.
/// @param width Positive scene width in pixels.
/// @param height Positive scene height in pixels.
/// @param threshold Linear-color brightness threshold applied during the first downsample.
/// @return Mip 0's texture name, or 0 when resources are unavailable.
static GLuint gl_encode_bloom_chain(
    gl_context_t *ctx, GLuint source_tex, int32_t width, int32_t height, float threshold) {
    gl_framebuffer_state_t saved;

    if (!ctx || !source_tex || ensure_bloom_targets(ctx, width, height) != 0)
        return 0;

    gl_capture_framebuffer_state(&saved);
    gl.Disable(GL_SCISSOR_TEST);
    gl.Disable(GL_DEPTH_TEST);
    gl.Disable(GL_CULL_FACE);
    gl.Disable(GL_BLEND);
    gl.ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    gl.DepthMask(GL_FALSE);
    gl.PolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    gl.BindVertexArray(ctx->fullscreen_vao);

    for (int32_t i = 0; i < ctx->bloom_mip_count; i++) {
        GLuint src_tex = (i == 0) ? source_tex : ctx->bloom_mip_tex[i - 1];
        int32_t src_w = (i == 0) ? width : ctx->bloom_mip_w[i - 1];
        int32_t src_h = (i == 0) ? height : ctx->bloom_mip_h[i - 1];
        gl.UseProgram(ctx->bloom_down_program);
        gl.Uniform1f(ctx->bloom_down_uThreshold, threshold);
        gl.Uniform1i(ctx->bloom_down_uFirstPass, i == 0 ? 1 : 0);
        gl_bloom_pass_draw(ctx,
                           ctx->bloom_down_program,
                           src_tex,
                           src_w,
                           src_h,
                           ctx->bloom_mip_fbo[i],
                           ctx->bloom_mip_w[i],
                           ctx->bloom_mip_h[i],
                           ctx->bloom_down_uSrcInvSize,
                           ctx->bloom_down_uSrcTex);
    }

    gl.Enable(GL_BLEND);
    gl.BlendFunc(GL_ONE, GL_ONE);
    for (int32_t i = ctx->bloom_mip_count - 1; i >= 1; i--) {
        gl_bloom_pass_draw(ctx,
                           ctx->bloom_up_program,
                           ctx->bloom_mip_tex[i],
                           ctx->bloom_mip_w[i],
                           ctx->bloom_mip_h[i],
                           ctx->bloom_mip_fbo[i - 1],
                           ctx->bloom_mip_w[i - 1],
                           ctx->bloom_mip_h[i - 1],
                           ctx->bloom_up_uSrcInvSize,
                           ctx->bloom_up_uSrcTex);
    }
    gl.Disable(GL_BLEND);

    if (!gl_drain_errors("bloom chain")) {
        gl_restore_framebuffer_state(&saved);
        gl_restore_main_draw_state(ctx);
        return 0;
    }
    gl_restore_framebuffer_state(&saved);
    gl_restore_main_draw_state(ctx);
    return ctx->bloom_mip_tex[0];
}

/// @brief Encode the Plan 05 TAA resolve into the free history slot and flip parity.
/// @details Reprojects + neighborhood-clamps the persistent history against
///          @p source_tex; the written slot becomes both the pass output and the next
///          frame's history.
/// @param ctx Borrowed initialized OpenGL context containing TAA resources and camera history.
/// @param source_tex Current scene-color texture to resolve.
/// @param width Positive source width and history-target width in pixels.
/// @param height Positive source height and history-target height in pixels.
/// @param snapshot Optional sanitized post-FX settings; null selects the default blend factor.
/// @return The resolved texture name, or 0 when resources are unavailable.
static GLuint gl_encode_taa_pass(gl_context_t *ctx,
                                 GLuint source_tex,
                                 int32_t width,
                                 int32_t height,
                                 const vgfx3d_postfx_snapshot_t *snapshot) {
    gl_framebuffer_state_t saved;
    int32_t write_slot;
    int32_t read_slot;
    float jitter_delta[2];

    if (!ctx || !source_tex || !ctx->scene_depth_tex || !ctx->scene_motion_tex ||
        ensure_taa_targets(ctx, width, height) != 0)
        return 0;

    write_slot = ctx->taa_history_parity ? 1 : 0;
    read_slot = 1 - write_slot;
    jitter_delta[0] = (ctx->taa_jitter_clip[0] - ctx->taa_prev_jitter_clip[0]) * 0.5f;
    jitter_delta[1] = (ctx->taa_jitter_clip[1] - ctx->taa_prev_jitter_clip[1]) * 0.5f;

    gl_capture_framebuffer_state(&saved);
    gl.BindFramebuffer(GL_FRAMEBUFFER, ctx->taa_history_fbo[write_slot]);
    gl.DrawBuffer(GL_COLOR_ATTACHMENT0);
    gl.Viewport(0, 0, width, height);
    gl.Disable(GL_SCISSOR_TEST);
    gl.Disable(GL_DEPTH_TEST);
    gl.Disable(GL_CULL_FACE);
    gl.Disable(GL_BLEND);
    gl.ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    gl.DepthMask(GL_FALSE);
    gl.PolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    gl.UseProgram(ctx->taa_program);
    gl.BindVertexArray(ctx->fullscreen_vao);
    bind_texture_unit(ctx->taa_uCurrTex, 0, GL_TEXTURE_2D, source_tex);
    bind_texture_unit(ctx->taa_uHistTex, 1, GL_TEXTURE_2D, ctx->taa_history_tex[read_slot]);
    bind_texture_unit(ctx->taa_uMotionTex, 2, GL_TEXTURE_2D, ctx->scene_motion_tex);
    bind_texture_unit(ctx->taa_uDepthTex, 3, GL_TEXTURE_2D, ctx->scene_depth_tex);
    gl.Uniform2f(ctx->taa_uInvResolution,
                 width > 0 ? 1.0f / (float)width : 0.0f,
                 height > 0 ? 1.0f / (float)height : 0.0f);
    gl.Uniform2f(ctx->taa_uJitterDelta, jitter_delta[0], jitter_delta[1]);
    gl.Uniform1f(ctx->taa_uBlend,
                 (snapshot && snapshot->taa_enabled && isfinite(snapshot->taa_blend))
                     ? snapshot->taa_blend
                     : 0.9f);
    gl.Uniform1i(ctx->taa_uHistoryValid,
                 (ctx->taa_history_valid && ctx->scene_history_valid) ? 1 : 0);
    gl.UniformMatrix4fv(ctx->taa_uInvViewProjection, 1, GL_TRUE, ctx->scene_inv_vp);
    gl.UniformMatrix4fv(ctx->taa_uPrevViewProjection, 1, GL_TRUE, ctx->scene_prev_vp);
    gl.DrawArrays(GL_TRIANGLES, 0, 3);

    if (!gl_drain_errors("taa resolve")) {
        gl_restore_framebuffer_state(&saved);
        gl_restore_main_draw_state(ctx);
        return 0;
    }
    gl_restore_framebuffer_state(&saved);
    gl_restore_main_draw_state(ctx);
    ctx->taa_history_parity = 1 - ctx->taa_history_parity;
    ctx->taa_history_valid = 1;
    return ctx->taa_history_tex[write_slot];
}

/// @brief Release the Plan 10 SSR output target.
/// @param ctx Borrowed OpenGL context whose SSR target is released; null is a no-op.
static void destroy_ssr_target(gl_context_t *ctx) {
    if (!ctx)
        return;
    if (ctx->ssr_tex)
        gl.DeleteTextures(1, &ctx->ssr_tex);
    if (ctx->ssr_fbo)
        gl.DeleteFramebuffers(1, &ctx->ssr_fbo);
    ctx->ssr_tex = 0;
    ctx->ssr_fbo = 0;
    ctx->ssr_width = 0;
    ctx->ssr_height = 0;
    ctx->ssr_hdr = 0;
}

/// @brief Build the Plan 10 SSR output target at the active scene precision.
/// @param ctx Borrowed initialized OpenGL context that owns the SSR objects.
/// @param w Positive scene width in pixels.
/// @param h Positive scene height in pixels.
/// @return Zero on success, -1 when allocation or completeness fails.
static int ensure_ssr_target(gl_context_t *ctx, int32_t w, int32_t h) {
    if (!ctx || !ctx->ssr_program)
        return -1;
    return gl_ensure_color_target(ctx,
                                  w,
                                  h,
                                  ctx->scene_hdr_active,
                                  &ctx->ssr_fbo,
                                  &ctx->ssr_tex,
                                  &ctx->ssr_width,
                                  &ctx->ssr_height,
                                  &ctx->ssr_hdr);
}

/// @brief Encode the Plan 10 SSR pass: ray-march the scene depth in screen space for
///        pixels whose motion-target alpha carries an SSR mask, compositing reflected
///        scene color over @p source_tex into the SSR target. Returns the composited
///        texture, or 0 when prerequisites are missing (caller keeps the source).
/// @param ctx Borrowed initialized OpenGL context containing SSR and scene resources.
/// @param source_tex Current scene-color texture sampled and composited by the pass.
/// @param width Positive source and destination width in pixels.
/// @param height Positive source and destination height in pixels.
/// @param snapshot Optional post-FX settings supplying SSR intensity, roughness, and step count.
/// @return The composited SSR texture name, or 0 when prerequisites are unavailable or drawing
///         reports an OpenGL error.
static GLuint gl_encode_ssr_pass(gl_context_t *ctx,
                                 GLuint source_tex,
                                 int32_t width,
                                 int32_t height,
                                 const vgfx3d_postfx_snapshot_t *snapshot) {
    gl_framebuffer_state_t saved;

    if (!ctx || !source_tex || !ctx->scene_depth_tex || !ctx->scene_motion_tex ||
        ensure_ssr_target(ctx, width, height) != 0)
        return 0;

    gl_capture_framebuffer_state(&saved);
    gl.BindFramebuffer(GL_FRAMEBUFFER, ctx->ssr_fbo);
    gl.DrawBuffer(GL_COLOR_ATTACHMENT0);
    gl.Viewport(0, 0, width, height);
    gl.Disable(GL_SCISSOR_TEST);
    gl.Disable(GL_DEPTH_TEST);
    gl.Disable(GL_CULL_FACE);
    gl.Disable(GL_BLEND);
    gl.ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    gl.DepthMask(GL_FALSE);
    gl.PolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    gl.UseProgram(ctx->ssr_program);
    gl.BindVertexArray(ctx->fullscreen_vao);
    bind_texture_unit(ctx->ssr_uSceneTex, 0, GL_TEXTURE_2D, source_tex);
    bind_texture_unit(ctx->ssr_uDepthTex, 1, GL_TEXTURE_2D, ctx->scene_depth_tex);
    bind_texture_unit(ctx->ssr_uMotionTex, 2, GL_TEXTURE_2D, ctx->scene_motion_tex);
    gl.Uniform2f(ctx->ssr_uInvResolution,
                 width > 0 ? 1.0f / (float)width : 0.0f,
                 height > 0 ? 1.0f / (float)height : 0.0f);
    gl.Uniform4f(ctx->ssr_uParams0,
                 (snapshot && isfinite(snapshot->ssr_intensity)) ? snapshot->ssr_intensity : 0.5f,
                 (snapshot && isfinite(snapshot->ssr_max_roughness)) ? snapshot->ssr_max_roughness
                                                                     : 0.4f,
                 (float)((snapshot && snapshot->ssr_steps > 0) ? snapshot->ssr_steps : 24),
                 0.0f);
    gl.Uniform3f(
        ctx->ssr_uCamPos, ctx->scene_cam_pos[0], ctx->scene_cam_pos[1], ctx->scene_cam_pos[2]);
    gl.UniformMatrix4fv(ctx->ssr_uInvViewProjection, 1, GL_TRUE, ctx->scene_inv_vp);
    gl.UniformMatrix4fv(ctx->ssr_uViewProjection, 1, GL_TRUE, ctx->scene_vp);
    gl.DrawArrays(GL_TRIANGLES, 0, 3);

    if (!gl_drain_errors("ssr pass")) {
        gl_restore_framebuffer_state(&saved);
        gl_restore_main_draw_state(ctx);
        return 0;
    }
    gl_restore_framebuffer_state(&saved);
    gl_restore_main_draw_state(ctx);
    return ctx->ssr_tex;
}

/// @brief Release RTT FBO + color texture + depth renderbuffer.
///
/// If the user-visible RT target has dirty pixels, syncs them back to
/// the RT's host buffer first via `gl_sync_render_target_color`.
/// Then signals end-of-render to anything waiting (`clear_sync`).
/// @param ctx Borrowed OpenGL context whose RTT objects and synchronization binding are released;
///            null is a no-op.
static void destroy_rtt_targets(gl_context_t *ctx) {
    if (!ctx)
        return;
    if (ctx->rtt_target) {
        int sync_ok = 1;
        if (ctx->rtt_color_dirty || ctx->rtt_target->color_dirty)
            sync_ok = gl_sync_render_target_color(ctx, ctx->rtt_target);
        if (sync_ok || !ctx->rtt_target->color_dirty)
            vgfx3d_rendertarget_clear_sync(ctx->rtt_target);
        else
            vgfx3d_rendertarget_detach_sync_preserve_dirty(ctx->rtt_target);
    }
    if (ctx->rtt_depth_rbo)
        gl.DeleteRenderbuffers(1, &ctx->rtt_depth_rbo);
    if (ctx->rtt_color_tex)
        gl.DeleteTextures(1, &ctx->rtt_color_tex);
    if (ctx->rtt_fbo)
        gl.DeleteFramebuffers(1, &ctx->rtt_fbo);
    ctx->rtt_fbo = 0;
    ctx->rtt_color_tex = 0;
    ctx->rtt_depth_rbo = 0;
    ctx->rtt_width = 0;
    ctx->rtt_height = 0;
    ctx->rtt_color_format = (int32_t)VGFX3D_RENDERTARGET_COLOR_FORMAT_UNORM8;
    ctx->rtt_active = 0;
    ctx->rtt_color_dirty = 0;
    ctx->rtt_target = NULL;
}

/// @brief Build / re-bind RTT targets sized to `rt`.
///
/// Idempotent on identical size + receiver. Switching to a different
/// `rt` of the same size syncs the previous RT first. Uses a renderbuffer
/// (not texture) for depth — RTT users don't need to read depth back
/// from the shader, so renderbuffer is faster.
/// @param ctx Borrowed initialized OpenGL context that owns the RTT objects.
/// @param rt Borrowed runtime render target that receives the backend synchronization callback.
/// @return 0 when the target is active and complete, otherwise -1 for invalid input or GL failure.
static int ensure_rtt_targets(gl_context_t *ctx, vgfx3d_rendertarget_t *rt) {
    gl_framebuffer_state_t saved;
    GLuint candidate_fbo = 0;
    GLuint candidate_color_tex = 0;
    GLuint candidate_depth_rbo = 0;
    GLuint previous_fbo;
    GLuint previous_color_tex;
    GLuint previous_depth_rbo;
    vgfx3d_rendertarget_t *previous_target;
    GLenum color_internal_format;
    GLenum depth_internal_format;
    GLenum color_type;

    if (!ctx || !rt || !vgfx3d_rendertarget_valid_pixels(rt, NULL) ||
        !gl_target_extent_is_valid(ctx, rt->width, rt->height) ||
        (rt->color_format != (int32_t)VGFX3D_RENDERTARGET_COLOR_FORMAT_UNORM8 &&
         rt->color_format != (int32_t)VGFX3D_RENDERTARGET_COLOR_FORMAT_HDR16F) ||
        (vgfx3d_rendertarget_is_hdr(rt) && !ctx->supports_hdr_color_target)) {
        return -1;
    }
    if (ctx->rtt_fbo && ctx->rtt_color_tex && ctx->rtt_depth_rbo && ctx->rtt_width == rt->width &&
        ctx->rtt_height == rt->height && ctx->rtt_color_format == rt->color_format) {
        if (ctx->rtt_target == rt) {
            ctx->rtt_active = 1;
            rt->sync_color = gl_sync_render_target_color;
            rt->sync_color_userdata = ctx;
            return 0;
        }
        if (ctx->rtt_target && ctx->rtt_target != rt) {
            if ((ctx->rtt_color_dirty || ctx->rtt_target->color_dirty) &&
                !gl_sync_render_target_color(ctx, ctx->rtt_target)) {
                return -1;
            }
            vgfx3d_rendertarget_clear_sync(ctx->rtt_target);
        }
        ctx->rtt_active = 1;
        ctx->rtt_target = rt;
        ctx->rtt_color_dirty = 0;
        rt->color_dirty = 0;
        rt->sync_color = gl_sync_render_target_color;
        rt->sync_color_userdata = ctx;
        return 0;
    }

    gl_capture_framebuffer_state(&saved);
    (void)gl_drain_errors("pre-rtt-target-allocation");
    gl.GenFramebuffers(1, &candidate_fbo);
    gl.BindFramebuffer(GL_FRAMEBUFFER, candidate_fbo);

    gl.GenTextures(1, &candidate_color_tex);
    gl.BindTexture(GL_TEXTURE_2D, candidate_color_tex);
    color_internal_format = vgfx3d_rendertarget_is_hdr(rt) ? GL_RGBA16F : GL_RGBA8;
    color_type = color_internal_format == GL_RGBA16F ? GL_FLOAT : GL_UNSIGNED_BYTE;
    depth_internal_format =
        ctx->supports_depth_float_target ? GL_DEPTH_COMPONENT32F : GL_DEPTH_COMPONENT24;
    gl.TexImage2D(GL_TEXTURE_2D,
                  0,
                  color_internal_format,
                  rt->width,
                  rt->height,
                  0,
                  GL_RGBA,
                  color_type,
                  NULL);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl.FramebufferTexture2D(
        GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, candidate_color_tex, 0);
    gl.DrawBuffer(GL_COLOR_ATTACHMENT0);
    gl.ReadBuffer(GL_COLOR_ATTACHMENT0);

    gl.GenRenderbuffers(1, &candidate_depth_rbo);
    gl.BindRenderbuffer(GL_RENDERBUFFER, candidate_depth_rbo);
    gl.RenderbufferStorage(GL_RENDERBUFFER, depth_internal_format, rt->width, rt->height);
    gl.FramebufferRenderbuffer(
        GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, candidate_depth_rbo);

    if (!candidate_fbo || !candidate_color_tex || !candidate_depth_rbo ||
        gl.CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE || !gl_upload_ok()) {
        if (candidate_depth_rbo)
            gl.DeleteRenderbuffers(1, &candidate_depth_rbo);
        if (candidate_color_tex)
            gl.DeleteTextures(1, &candidate_color_tex);
        if (candidate_fbo)
            gl.DeleteFramebuffers(1, &candidate_fbo);
        gl_restore_framebuffer_state(&saved);
        return -1;
    }

    previous_fbo = ctx->rtt_fbo;
    previous_color_tex = ctx->rtt_color_tex;
    previous_depth_rbo = ctx->rtt_depth_rbo;
    previous_target = ctx->rtt_target;
    if (previous_target && (ctx->rtt_color_dirty || previous_target->color_dirty) &&
        !gl_sync_render_target_color(ctx, previous_target)) {
        gl.DeleteRenderbuffers(1, &candidate_depth_rbo);
        gl.DeleteTextures(1, &candidate_color_tex);
        gl.DeleteFramebuffers(1, &candidate_fbo);
        gl_restore_framebuffer_state(&saved);
        return -1;
    }
    if (previous_target)
        vgfx3d_rendertarget_clear_sync(previous_target);
    if ((GLuint)saved.draw_framebuffer == previous_fbo)
        saved.draw_framebuffer = (GLint)candidate_fbo;
    if ((GLuint)saved.read_framebuffer == previous_fbo)
        saved.read_framebuffer = (GLint)candidate_fbo;
    if ((GLuint)saved.texture_binding_2d == previous_color_tex)
        saved.texture_binding_2d = (GLint)candidate_color_tex;
    if ((GLuint)saved.renderbuffer == previous_depth_rbo)
        saved.renderbuffer = (GLint)candidate_depth_rbo;
    ctx->rtt_fbo = candidate_fbo;
    ctx->rtt_color_tex = candidate_color_tex;
    ctx->rtt_depth_rbo = candidate_depth_rbo;
    ctx->rtt_width = rt->width;
    ctx->rtt_height = rt->height;
    ctx->rtt_color_format = rt->color_format;
    ctx->rtt_active = 1;
    ctx->rtt_target = rt;
    ctx->rtt_color_dirty = 0;
    rt->color_dirty = 0;
    rt->sync_color = gl_sync_render_target_color;
    rt->sync_color_userdata = ctx;
    if (previous_depth_rbo)
        gl.DeleteRenderbuffers(1, &previous_depth_rbo);
    if (previous_color_tex)
        gl.DeleteTextures(1, &previous_color_tex);
    if (previous_fbo)
        gl.DeleteFramebuffers(1, &previous_fbo);
    gl_restore_framebuffer_state(&saved);
    return 0;
}

/// @brief Release the shadow-map FBO + depth texture.
/// @param ctx Borrowed OpenGL context whose complete shadow-target array is released; null is a
///            no-op.
static void destroy_shadow_class(gl_context_t *ctx, int32_t group) {
    if (!ctx)
        return;
    int32_t first = group == 0 ? 0 : VGFX3D_CSM_SLOTS;
    int32_t end = group == 0 ? VGFX3D_CSM_SLOTS : VGFX3D_MAX_SHADOW_LIGHTS;
    for (int32_t slot = first; slot < end; ++slot) {
        if (ctx->shadow_fbo[slot])
            gl.DeleteFramebuffers(1, &ctx->shadow_fbo[slot]);
        ctx->shadow_fbo[slot] = 0;
        ctx->shadow_width[slot] = ctx->shadow_height[slot] = 0;
        ctx->shadow_complete[slot] = 0;
    }
    if (ctx->shadow_array_tex[group])
        gl.DeleteTextures(1, &ctx->shadow_array_tex[group]);
    ctx->shadow_array_tex[group] = 0;
    ctx->shadow_array_width[group] = ctx->shadow_array_height[group] = 0;
    ctx->shadow_array_layers[group] = 0;
}

/// @brief Release both independently sized shadow array classes.
/// @param ctx Borrowed context; null is ignored.
static void destroy_shadow_targets(gl_context_t *ctx) {
    if (!ctx)
        return;
    destroy_shadow_class(ctx, 0);
    destroy_shadow_class(ctx, 1);
    ctx->shadow_pass_slot = -1;
    ctx->shadow_count = 0;
}

/// @brief Recount the contiguous fully-prepared shadow slots advertised to GLSL.
///
/// The shader receives a single `shadowCount` upper bound for the sampler array. Keeping that
/// count to the completed prefix starting at slot 0 makes `0 <= shadowIndex < shadowCount`
/// equivalent to "a complete GL shadow texture exists", matching the D3D11 backend contract.
/// @param ctx Borrowed OpenGL context whose advertised shadow count is recomputed; null is a
///            no-op.
static void gl_recompute_shadow_count(gl_context_t *ctx) {
    int32_t count = 0;
    if (!ctx)
        return;
    for (int32_t slot = 0; slot < VGFX3D_MAX_SHADOW_LIGHTS; slot++) {
        if (!ctx->shadow_complete[slot] || !ctx->shadow_array_tex[slot >= VGFX3D_CSM_SLOTS] ||
            !ctx->shadow_fbo[slot] || ctx->shadow_width[slot] <= 0 || ctx->shadow_height[slot] <= 0)
            break;
        count++;
    }
    ctx->shadow_count = count;
}

/// @brief Validate a material light's requested shadow texture slot against real GL resources.
///
/// This remains a direct resource guard even though `uShadowCount` is now contiguous, so a failed
/// resize or allocation cannot leave a light sampling an incomplete shadow FBO.
/// @param ctx Borrowed OpenGL context containing the shadow-slot resource state.
/// @param shadow_index Material-requested shadow index to validate.
/// @return A complete shadow slot in `[0, VGFX3D_MAX_SHADOW_LIGHTS)`, or -1 when invalid.
static int32_t gl_sanitize_shadow_slot(gl_context_t *ctx, int32_t shadow_index) {
    int32_t slot;
    if (!ctx)
        return -1;
    slot = vgfx3d_opengl_sanitize_shadow_index(shadow_index, ctx->shadow_count);
    if (slot < 0 || slot >= VGFX3D_MAX_SHADOW_LIGHTS)
        return -1;
    if (!ctx->shadow_complete[slot] || !ctx->shadow_array_tex[slot >= VGFX3D_CSM_SLOTS] ||
        !ctx->shadow_fbo[slot] || ctx->shadow_width[slot] <= 0 || ctx->shadow_height[slot] <= 0) {
        return -1;
    }
    return slot;
}

/// @brief Build a depth-only FBO for shadow-map rendering.
///
/// `glDrawBuffer(GL_NONE)` + `glReadBuffer(GL_NONE)` because there are
/// no color attachments (depth-only). Linear filtering on the depth
/// texture so the main pass can use hardware PCF when sampling.
/// @param ctx Borrowed initialized OpenGL context that owns the shadow objects.
/// @param slot Shadow-resource array slot to allocate or reuse.
/// @param w Positive shadow-map width in pixels.
/// @param h Positive shadow-map height in pixels.
/// @return 0 when a complete depth-only framebuffer is ready, otherwise -1.
static int ensure_shadow_targets(gl_context_t *ctx, int32_t slot, int32_t w, int32_t h) {
    if (!ctx)
        return -1;
    const int32_t group = slot >= VGFX3D_CSM_SLOTS;
    const int32_t layer = group ? slot - VGFX3D_CSM_SLOTS : slot;
    int32_t layers = ctx->shadow_requested_layers[group];
    if (layers <= 0)
        layers = group ? VGFX3D_MAX_SHADOW_LIGHTS - VGFX3D_CSM_SLOTS : VGFX3D_CSM_SLOTS;
    if (layer < 0 || layer >= layers)
        return -1;
    gl_framebuffer_state_t saved;
    GLuint candidate_fbo = 0;
    GLuint previous_fbo;
    GLint depth_format;
    GLenum depth_type;
    if (!gl_target_extent_is_valid(ctx, w, h) || slot < 0 || slot >= VGFX3D_MAX_SHADOW_LIGHTS) {
        return -1;
    }
    /* The preparation hook allocates each class before any depth is rendered. */
    if (ctx->shadow_array_tex[group] && ctx->shadow_fbo[slot] &&
        ctx->shadow_array_width[group] == w && ctx->shadow_array_height[group] == h &&
        ctx->shadow_array_layers[group] == layers)
        return 0;

    depth_format = ctx->supports_depth_float_target ? GL_DEPTH_COMPONENT32F : GL_DEPTH_COMPONENT24;
    depth_type = ctx->supports_depth_float_target ? GL_FLOAT : GL_UNSIGNED_INT;

    gl_capture_framebuffer_state(&saved);
    (void)gl_drain_errors("pre-shadow-target-allocation");

    /* Growing invalidates every layer at once, so drop the whole array and all
     * per-slot FBOs rather than leaving slots pointing at a stale texture. */
    if (ctx->shadow_array_tex[group] &&
        (ctx->shadow_array_width[group] != w || ctx->shadow_array_height[group] != h ||
         ctx->shadow_array_layers[group] != layers)) {
        destroy_shadow_class(ctx, group);
    }

    if (!ctx->shadow_array_tex[group]) {
        GLuint candidate_array = 0;
        gl.GenTextures(1, &candidate_array);
        gl.BindTexture(GL_TEXTURE_2D_ARRAY, candidate_array);
        gl.TexImage3D(GL_TEXTURE_2D_ARRAY,
                      0,
                      depth_format,
                      w,
                      h,
                      (GLsizei)layers,
                      0,
                      GL_DEPTH_COMPONENT,
                      depth_type,
                      NULL);
        gl.TexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        gl.TexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        gl.TexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        gl.TexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        /* Sampled through sampler2DArrayShadow: the comparison mode makes each
         * texture() tap a hardware-filtered depth compare (free 2x2 PCF per tap). */
        gl.TexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
        gl.TexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
        if (!candidate_array || !gl_upload_ok()) {
            if (candidate_array)
                gl.DeleteTextures(1, &candidate_array);
            gl_restore_framebuffer_state(&saved);
            return -1;
        }
        ctx->shadow_array_tex[group] = candidate_array;
        ctx->shadow_array_width[group] = w;
        ctx->shadow_array_height[group] = h;
        ctx->shadow_array_layers[group] = layers;
    }

    gl.GenFramebuffers(1, &candidate_fbo);
    gl.BindFramebuffer(GL_FRAMEBUFFER, candidate_fbo);
    gl.FramebufferTextureLayer(
        GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, ctx->shadow_array_tex[group], 0, (GLint)layer);
    gl.DrawBuffer(GL_NONE);
    gl.ReadBuffer(GL_NONE);
    if (!candidate_fbo || gl.CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE ||
        !gl_upload_ok()) {
        if (candidate_fbo)
            gl.DeleteFramebuffers(1, &candidate_fbo);
        gl_restore_framebuffer_state(&saved);
        return -1;
    }

    previous_fbo = ctx->shadow_fbo[slot];
    if ((GLuint)saved.draw_framebuffer == previous_fbo)
        saved.draw_framebuffer = (GLint)candidate_fbo;
    if ((GLuint)saved.read_framebuffer == previous_fbo)
        saved.read_framebuffer = (GLint)candidate_fbo;
    ctx->shadow_fbo[slot] = candidate_fbo;
    ctx->shadow_width[slot] = w;
    ctx->shadow_height[slot] = h;
    ctx->shadow_complete[slot] = 0;
    gl_recompute_shadow_count(ctx);
    if (previous_fbo)
        gl.DeleteFramebuffers(1, &previous_fbo);
    gl_restore_framebuffer_state(&saved);
    return 0;
}

/// @brief Bind whichever framebuffer the active target kind says is current.
///
/// Three-way dispatch: RTT → user RTT FBO; SCENE → offscreen MRT FBO
/// with both color attachments enabled; else → swapchain (FBO 0).
/// Sets the matching viewport dimensions.
/// @param ctx Borrowed OpenGL context whose active-target state selects the binding.
static void bind_main_framebuffer(gl_context_t *ctx) {
    if (ctx->rtt_active) {
        gl.BindFramebuffer(GL_FRAMEBUFFER, ctx->rtt_fbo);
        gl.DrawBuffer(GL_COLOR_ATTACHMENT0);
        gl.ReadBuffer(GL_COLOR_ATTACHMENT0);
        gl.Viewport(0, 0, ctx->rtt_width, ctx->rtt_height);
    } else if (ctx->active_target_kind == VGFX3D_OPENGL_TARGET_SWAPCHAIN) {
        if (!ctx->default_framebuffer_writable && ctx->scene_composited_to_backbuffer &&
            ctx->present_fbo) {
            gl.BindFramebuffer(GL_FRAMEBUFFER, ctx->present_fbo);
            gl.DrawBuffer(GL_COLOR_ATTACHMENT0);
            gl.ReadBuffer(GL_COLOR_ATTACHMENT0);
        } else if (!ctx->default_framebuffer_writable && ctx->scene_fbo) {
            gl.BindFramebuffer(GL_FRAMEBUFFER, ctx->scene_fbo);
            gl.DrawBuffer(GL_COLOR_ATTACHMENT0);
            gl.ReadBuffer(GL_COLOR_ATTACHMENT0);
        } else {
            gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
            gl.DrawBuffer(ctx->default_draw_buffer);
            gl.ReadBuffer(ctx->default_draw_buffer);
        }
        gl.Viewport(0, 0, ctx->width, ctx->height);
    } else {
        gl.BindFramebuffer(GL_FRAMEBUFFER, ctx->scene_fbo);
        {
            const GLenum attachments[2] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
            gl.DrawBuffers(2, attachments);
        }
        gl.ReadBuffer(GL_COLOR_ATTACHMENT0);
        gl.Viewport(0, 0, ctx->scene_width, ctx->scene_height);
    }
}

/// @brief Per-draw blend / depth / motion-attachment setup.
///
/// Picks blend mode (alpha or opaque) + depth-write toggle + motion
/// attachment count from the draw command. The motion attachment is
/// only enabled in the SCENE target with the right blend mode — we
/// don't emit motion vectors for transparent passes since they'd
/// just overwrite opaque results.
/// @param ctx Borrowed OpenGL context whose framebuffer and draw state are configured.
/// @param cmd Optional draw command supplying blend and depth-test behavior.
static void gl_configure_draw_output(gl_context_t *ctx, const vgfx3d_draw_cmd_t *cmd) {
    vgfx3d_opengl_blend_mode_t blend_mode;
    vgfx3d_opengl_motion_attachment_mode_t motion_mode;

    if (!ctx)
        return;
    blend_mode = vgfx3d_opengl_choose_blend_mode(cmd);
    motion_mode = vgfx3d_opengl_choose_motion_attachment_mode(ctx->active_target_kind, cmd);

    // Additive light carries radiance, not new coverage in a transparent overlay.
    gl.ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, blend_mode != VGFX3D_OPENGL_BLEND_ADDITIVE);

    if (blend_mode == VGFX3D_OPENGL_BLEND_ALPHA || blend_mode == VGFX3D_OPENGL_BLEND_ADDITIVE)
        gl.Enable(GL_BLEND);
    else
        gl.Disable(GL_BLEND);
    if (blend_mode == VGFX3D_OPENGL_BLEND_ADDITIVE)
        gl.BlendFunc(GL_SRC_ALPHA, GL_ONE);
    else
        gl.BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    if (cmd && cmd->disable_depth_test) {
        gl.Disable(GL_DEPTH_TEST);
        gl.DepthMask(GL_FALSE);
    } else {
        gl.Enable(GL_DEPTH_TEST);
        /* Reversed-Z: GL_GREATER mirrors Metal/D3D11 — first-writer-wins on coplanar
         * geometry under the reversed convention (near = 1, far = 0). The skybox pass
         * alone uses GEQUAL to pass at the cleared far depth of 0. */
        gl.DepthFunc(GL_GREATER);
        gl.DepthMask(blend_mode == VGFX3D_OPENGL_BLEND_OPAQUE ? GL_TRUE : GL_FALSE);
    }

    if (ctx->active_target_kind == VGFX3D_OPENGL_TARGET_SCENE) {
        gl.BindFramebuffer(GL_FRAMEBUFFER, ctx->scene_fbo);
        if (motion_mode == VGFX3D_OPENGL_MOTION_ATTACHMENTS_COLOR_AND_MOTION) {
            const GLenum attachments[2] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
            gl.DrawBuffers(2, attachments);
        } else {
            gl.DrawBuffer(GL_COLOR_ATTACHMENT0);
        }
        gl.Viewport(0, 0, ctx->scene_width, ctx->scene_height);
        return;
    }
    if (ctx->active_target_kind == VGFX3D_OPENGL_TARGET_RTT) {
        gl.BindFramebuffer(GL_FRAMEBUFFER, ctx->rtt_fbo);
        gl.DrawBuffer(GL_COLOR_ATTACHMENT0);
        gl.Viewport(0, 0, ctx->rtt_width, ctx->rtt_height);
        return;
    }
    if (!ctx->default_framebuffer_writable && ctx->scene_composited_to_backbuffer &&
        ctx->present_fbo) {
        gl.BindFramebuffer(GL_FRAMEBUFFER, ctx->present_fbo);
        gl.DrawBuffer(GL_COLOR_ATTACHMENT0);
    } else if (!ctx->default_framebuffer_writable && ctx->scene_fbo) {
        gl.BindFramebuffer(GL_FRAMEBUFFER, ctx->scene_fbo);
        gl.DrawBuffer(GL_COLOR_ATTACHMENT0);
    } else {
        gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
        gl.DrawBuffer(ctx->default_draw_buffer);
    }
    gl.Viewport(0, 0, ctx->width, ctx->height);
}

/// @brief Apply per-material polygon offset for coplanar geometry.
/// @details OpenGL keeps polygon offset as sticky global state, so every draw explicitly enables
///   and configures it or disables it. The command contract uses negative constant values to pull
///   triangles toward the camera and positive values to push them away. Scene passes render
///   reversed-Z (`GL_GREATER`), which inverts the window-z direction, so both offset terms are
///   sign-flipped there; the shadow pass keeps the standard convention and passes
///   `reversed_z == 0`.
/// @param cmd Optional draw command containing constant and slope-scaled bias values.
/// @param reversed_z Non-zero when the active pass uses reversed-Z depth semantics.
static void gl_apply_depth_bias(const vgfx3d_draw_cmd_t *cmd, int reversed_z) {
    if (cmd && (fabsf(cmd->depth_bias) > 1e-8f || fabsf(cmd->slope_scaled_depth_bias) > 1e-8f)) {
        gl.Enable(GL_POLYGON_OFFSET_FILL);
        if (reversed_z)
            gl.PolygonOffset(vgfx3d_depth_bias_slope_reversed_z(cmd->slope_scaled_depth_bias),
                             vgfx3d_depth_bias_constant_units_reversed_z(cmd->depth_bias));
        else
            gl.PolygonOffset(cmd->slope_scaled_depth_bias,
                             vgfx3d_depth_bias_constant_units(cmd->depth_bias));
    } else {
        gl.Disable(GL_POLYGON_OFFSET_FILL);
    }
}

/// @brief Push every post-FX uniform from the snapshot into the program.
///
/// Each toggle has a paired enable flag + parameter set: bloom,
/// tonemap, FXAA, color grading, vignette, SSAO, DoF, motion blur.
/// NULL snapshot zeros every toggle (post-FX disabled). The camera
/// + inverse VP + previous VP go through unconditionally so motion-
/// blur and SSAO can do their world-space reconstructions.
/// @param ctx Borrowed OpenGL context containing the active program's uniform locations.
/// @param snapshot Optional effect settings; null disables all configurable effects.
/// @param source_width Source texture width used to compute inverse resolution.
/// @param source_height Source texture height used to compute inverse resolution.
static void gl_apply_postfx_uniforms(gl_context_t *ctx,
                                     const vgfx3d_postfx_snapshot_t *snapshot,
                                     int32_t source_width,
                                     int32_t source_height) {
    int32_t bloom_passes;
    int32_t ssao_samples;
    int32_t motion_samples;
    vgfx3d_postfx_snapshot_t safe_snapshot;
    if (!ctx)
        return;
    if (snapshot) {
        vgfx3d_sanitize_postfx_snapshot(snapshot, &safe_snapshot);
        snapshot = &safe_snapshot;
    }
    gl.Uniform2f(ctx->postfx_uInvResolution,
                 source_width > 0 ? 1.0f / (float)source_width : 0.0f,
                 source_height > 0 ? 1.0f / (float)source_height : 0.0f);
    gl.Uniform3f(ctx->postfx_uCameraPos,
                 ctx->scene_cam_pos[0],
                 ctx->scene_cam_pos[1],
                 ctx->scene_cam_pos[2]);
    gl.UniformMatrix4fv(ctx->postfx_uInvViewProjection, 1, GL_TRUE, ctx->scene_inv_vp);
    gl.UniformMatrix4fv(ctx->postfx_uPrevViewProjection, 1, GL_TRUE, ctx->scene_prev_vp);
    /* ADR 0247: the scene target is scene-referred linear; precision only
     * determines whether values above 1.0 survive until the tone curve. */
    gl.Uniform1i(ctx->postfx_uSceneHdr, ctx->scene_hdr_active ? 1 : 0);
    gl.Uniform1i(ctx->postfx_uBloomTexEnabled,
                 (snapshot && snapshot->bloom_enabled && ctx->postfx_current_bloom_tex) ? 1 : 0);

    if (!snapshot) {
        gl.Uniform1i(ctx->postfx_uTonemapExplicit, 0);
        gl.Uniform1i(ctx->postfx_uBloomEnabled, 0);
        gl.Uniform1f(ctx->postfx_uBloomThreshold, 1.0f);
        gl.Uniform1f(ctx->postfx_uBloomIntensity, 0.0f);
        gl.Uniform1i(ctx->postfx_uBloomPasses, 0);
        gl.Uniform1i(ctx->postfx_uTonemapMode, 0);
        gl.Uniform1f(ctx->postfx_uTonemapExposure, 1.0f);
        gl.Uniform1i(ctx->postfx_uFxaaEnabled, 0);
        gl.Uniform1i(ctx->postfx_uColorGradeEnabled, 0);
        gl.Uniform1f(ctx->postfx_uCgBrightness, 0.0f);
        gl.Uniform1f(ctx->postfx_uCgContrast, 1.0f);
        gl.Uniform1f(ctx->postfx_uCgSaturation, 1.0f);
        gl.Uniform1i(ctx->postfx_uVignetteEnabled, 0);
        gl.Uniform1f(ctx->postfx_uVignetteRadius, 1.0f);
        gl.Uniform1f(ctx->postfx_uVignetteSoftness, 0.0f);
        gl.Uniform1i(ctx->postfx_uSsaoEnabled, 0);
        gl.Uniform1f(ctx->postfx_uSsaoRadius, 0.0f);
        gl.Uniform1f(ctx->postfx_uSsaoIntensity, 0.0f);
        gl.Uniform1i(ctx->postfx_uSsaoSamples, 0);
        gl.Uniform1i(ctx->postfx_uDofEnabled, 0);
        gl.Uniform1f(ctx->postfx_uDofFocusDistance, 0.0f);
        gl.Uniform1f(ctx->postfx_uDofAperture, 0.0f);
        gl.Uniform1f(ctx->postfx_uDofMaxBlur, 0.0f);
        gl.Uniform1i(ctx->postfx_uMotionBlurEnabled, 0);
        gl.Uniform1f(ctx->postfx_uMotionBlurIntensity, 0.0f);
        gl.Uniform1i(ctx->postfx_uMotionBlurSamples, 0);
        gl.Uniform1i(ctx->postfx_uLutEnabled, 0);
        gl.Uniform1f(ctx->postfx_uLutBlend, 0.0f);
        gl.Uniform1i(ctx->postfx_uSharpenEnabled, 0);
        gl.Uniform1f(ctx->postfx_uSharpenAmount, 0.0f);
        return;
    }

    gl.Uniform1i(ctx->postfx_uTonemapExplicit, snapshot->tonemap_explicit ? 1 : 0);
    bloom_passes = snapshot->bloom_passes < 0 ? 0 : snapshot->bloom_passes;
    if (bloom_passes > 32)
        bloom_passes = 32;
    ssao_samples = snapshot->ssao_samples < 4 ? 4 : snapshot->ssao_samples;
    if (ssao_samples > 16)
        ssao_samples = 16; /* Plan 05 spiral kernel supports up to 16 taps */
    motion_samples = snapshot->motion_blur_samples < 2 ? 2 : snapshot->motion_blur_samples;
    if (motion_samples > 8)
        motion_samples = 8;
    gl.Uniform1i(ctx->postfx_uBloomEnabled, snapshot->bloom_enabled ? 1 : 0);
    gl.Uniform1f(ctx->postfx_uBloomThreshold,
                 isfinite(snapshot->bloom_threshold) ? snapshot->bloom_threshold : 1.0f);
    gl.Uniform1f(ctx->postfx_uBloomIntensity,
                 isfinite(snapshot->bloom_intensity) ? snapshot->bloom_intensity : 0.0f);
    gl.Uniform1i(ctx->postfx_uBloomPasses, bloom_passes);
    gl.Uniform1i(ctx->postfx_uTonemapMode, snapshot->tonemap_mode);
    gl.Uniform1f(ctx->postfx_uTonemapExposure,
                 isfinite(snapshot->tonemap_exposure) ? snapshot->tonemap_exposure : 1.0f);
    gl.Uniform1i(ctx->postfx_uFxaaEnabled, snapshot->fxaa_enabled ? 1 : 0);
    gl.Uniform1i(ctx->postfx_uColorGradeEnabled, snapshot->color_grade_enabled ? 1 : 0);
    gl.Uniform1f(ctx->postfx_uCgBrightness,
                 isfinite(snapshot->cg_brightness) ? snapshot->cg_brightness : 0.0f);
    gl.Uniform1f(ctx->postfx_uCgContrast,
                 isfinite(snapshot->cg_contrast) ? snapshot->cg_contrast : 1.0f);
    gl.Uniform1f(ctx->postfx_uCgSaturation,
                 isfinite(snapshot->cg_saturation) ? snapshot->cg_saturation : 1.0f);
    gl.Uniform1i(ctx->postfx_uVignetteEnabled, snapshot->vignette_enabled ? 1 : 0);
    gl.Uniform1f(ctx->postfx_uVignetteRadius,
                 isfinite(snapshot->vignette_radius) ? snapshot->vignette_radius : 1.0f);
    gl.Uniform1f(ctx->postfx_uVignetteSoftness,
                 isfinite(snapshot->vignette_softness) ? snapshot->vignette_softness : 0.0f);
    gl.Uniform1i(ctx->postfx_uSsaoEnabled, snapshot->ssao_enabled ? 1 : 0);
    gl.Uniform1f(ctx->postfx_uSsaoRadius,
                 isfinite(snapshot->ssao_radius) ? snapshot->ssao_radius : 0.0f);
    gl.Uniform1f(ctx->postfx_uSsaoIntensity,
                 isfinite(snapshot->ssao_intensity) ? snapshot->ssao_intensity : 0.0f);
    gl.Uniform1i(ctx->postfx_uSsaoSamples, ssao_samples);
    gl.Uniform1i(ctx->postfx_uDofEnabled, snapshot->dof_enabled ? 1 : 0);
    gl.Uniform1f(ctx->postfx_uDofFocusDistance,
                 isfinite(snapshot->dof_focus_distance) ? snapshot->dof_focus_distance : 0.0f);
    gl.Uniform1f(ctx->postfx_uDofAperture,
                 isfinite(snapshot->dof_aperture) ? snapshot->dof_aperture : 0.0f);
    gl.Uniform1f(ctx->postfx_uDofMaxBlur,
                 isfinite(snapshot->dof_max_blur) ? snapshot->dof_max_blur : 0.0f);
    gl.Uniform1i(ctx->postfx_uMotionBlurEnabled, snapshot->motion_blur_enabled ? 1 : 0);
    gl.Uniform1f(ctx->postfx_uMotionBlurIntensity,
                 isfinite(snapshot->motion_blur_intensity) ? snapshot->motion_blur_intensity
                                                           : 0.0f);
    gl.Uniform1i(ctx->postfx_uMotionBlurSamples, motion_samples);
    /* Plan 61: LUT payload validity is re-checked at bind time
     * (gl_ensure_postfx_lut_texture); the flag alone rides the uniforms. */
    gl.Uniform1i(ctx->postfx_uLutEnabled, snapshot->color_lut_enabled ? 1 : 0);
    gl.Uniform1f(ctx->postfx_uLutBlend,
                 isfinite(snapshot->color_lut_blend) ? snapshot->color_lut_blend : 1.0f);
    gl.Uniform1i(ctx->postfx_uSharpenEnabled, snapshot->sharpen_enabled ? 1 : 0);
    gl.Uniform1f(ctx->postfx_uSharpenAmount,
                 isfinite(snapshot->sharpen_amount) ? snapshot->sharpen_amount : 0.0f);
}

/// @brief Return the cached 256x16 COLOR_LUT texture for @p snapshot, uploading when the
/// snapshot's (texel pointer, revision) cache key changes.
/// @details The texel pointer is borrowed from the chain-retained LUT Pixels and packed
///          0xRRGGBBAA per texel; bytes are swizzled into RGBA8 staging before upload so
///          host endianness never leaks into the texture. Returns 0 when the snapshot
///          carries no usable LUT payload or allocation fails.
/// @param ctx Borrowed OpenGL context owning the cache slot.
/// @param snapshot Borrowed effect snapshot, or NULL.
/// @return The ready GL texture name, or 0 when unavailable.
static GLuint gl_ensure_postfx_lut_texture(gl_context_t *ctx,
                                           const vgfx3d_postfx_snapshot_t *snapshot) {
    uint8_t staging[256 * 16 * 4];

    if (!ctx || !snapshot || !snapshot->color_lut_enabled || !snapshot->color_lut_texels ||
        snapshot->color_lut_width != 256 || snapshot->color_lut_height != 16)
        return 0;
    if (ctx->postfx_lut_tex && ctx->postfx_lut_key == (uintptr_t)snapshot->color_lut_texels &&
        ctx->postfx_lut_revision == snapshot->color_lut_revision)
        return ctx->postfx_lut_tex;
    if (!ctx->postfx_lut_tex)
        gl.GenTextures(1, &ctx->postfx_lut_tex);
    if (!ctx->postfx_lut_tex)
        return 0;
    for (int32_t i = 0; i < 256 * 16; i++) {
        uint32_t t = snapshot->color_lut_texels[i];
        staging[i * 4 + 0] = (uint8_t)((t >> 24) & 0xFFu);
        staging[i * 4 + 1] = (uint8_t)((t >> 16) & 0xFFu);
        staging[i * 4 + 2] = (uint8_t)((t >> 8) & 0xFFu);
        staging[i * 4 + 3] = (uint8_t)(t & 0xFFu);
    }
    gl.BindTexture(GL_TEXTURE_2D, ctx->postfx_lut_tex);
    gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 256, 16, 0, GL_RGBA, GL_UNSIGNED_BYTE, staging);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl.BindTexture(GL_TEXTURE_2D, 0);
    ctx->postfx_lut_key = (uintptr_t)snapshot->color_lut_texels;
    ctx->postfx_lut_revision = snapshot->color_lut_revision;
    return ctx->postfx_lut_tex;
}

/// @brief Composite the offscreen scene texture onto a target FBO via post-FX.
///
/// Binds the postfx program + fullscreen-triangle VAO, plus scene
/// color/depth/motion as samplers, applies post-FX uniforms, draws
/// 3 vertices (the standard "fullscreen triangle covers the viewport"
/// trick — cheaper than a 6-vertex quad).
/// @param ctx Borrowed initialized OpenGL context containing post-FX resources.
/// @param source_color_tex Texture containing the color input for the pass.
/// @param framebuffer Destination framebuffer, where zero selects the default framebuffer.
/// @param draw_buffer Draw-buffer enum selected on @p framebuffer.
/// @param source_width Positive source texture width in pixels.
/// @param source_height Positive source texture height in pixels.
/// @param target_width Positive destination viewport width in pixels.
/// @param target_height Positive destination viewport height in pixels.
/// @param snapshot Optional effect settings; null performs a plain post-FX shader copy.
/// @return Non-zero when the full-screen draw completes without a drained GL error, otherwise 0.
static int gl_draw_texture_to_target(gl_context_t *ctx,
                                     GLuint source_color_tex,
                                     GLuint framebuffer,
                                     GLenum draw_buffer,
                                     int32_t source_width,
                                     int32_t source_height,
                                     int32_t target_width,
                                     int32_t target_height,
                                     const vgfx3d_postfx_snapshot_t *snapshot) {
    gl_framebuffer_state_t saved;
    int ok;

    if (!ctx || !ctx->postfx_program || !source_color_tex || source_width <= 0 ||
        source_height <= 0 || target_width <= 0 || target_height <= 0)
        return 0;

    gl_capture_framebuffer_state(&saved);
    gl.BindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    gl.DrawBuffer(draw_buffer);
    gl.ReadBuffer(draw_buffer);
    gl.Viewport(0, 0, target_width, target_height);
    gl.Disable(GL_SCISSOR_TEST);
    gl.Disable(GL_DEPTH_TEST);
    gl.Disable(GL_CULL_FACE);
    gl.Disable(GL_BLEND);
    gl.ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    gl.DepthMask(GL_FALSE);
    gl.PolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    gl.UseProgram(ctx->postfx_program);
    gl.BindVertexArray(ctx->fullscreen_vao);
    gl.Uniform1i(ctx->postfx_uSceneTex, 0);
    gl.Uniform1i(ctx->postfx_uSceneDepthTex, 1);
    gl.Uniform1i(ctx->postfx_uSceneMotionTex, 2);
    gl.Uniform1i(ctx->postfx_uBloomTex, 3);
    bind_texture_unit(ctx->postfx_uSceneTex, 0, GL_TEXTURE_2D, source_color_tex);
    bind_texture_unit(ctx->postfx_uSceneDepthTex, 1, GL_TEXTURE_2D, ctx->scene_depth_tex);
    bind_texture_unit(ctx->postfx_uSceneMotionTex, 2, GL_TEXTURE_2D, ctx->scene_motion_tex);
    bind_texture_unit(ctx->postfx_uBloomTex,
                      3,
                      GL_TEXTURE_2D,
                      ctx->postfx_current_bloom_tex ? ctx->postfx_current_bloom_tex
                                                    : ctx->default_white_tex);
    gl_apply_postfx_uniforms(ctx, snapshot, source_width, source_height);
    {
        /* Plan 61: bind the cached LUT strip; a failed upload force-disables
         * the pass so the shader never grades toward the fallback texture. */
        GLuint lut_tex = gl_ensure_postfx_lut_texture(ctx, snapshot);
        gl.Uniform1i(ctx->postfx_uLutTex, 4);
        gl.Uniform1i(ctx->postfx_uLutEnabled, lut_tex ? 1 : 0);
        bind_texture_unit(
            ctx->postfx_uLutTex, 4, GL_TEXTURE_2D, lut_tex ? lut_tex : ctx->default_white_tex);
    }
    gl.DrawArrays(GL_TRIANGLES, 0, 3);
    ok = gl_drain_errors("postfx draw");
    gl_restore_framebuffer_state(&saved);
    gl_restore_main_draw_state(ctx);
    return ok;
}

/// @brief Apply post-FX inside the scene FBO when standalone final FBOs are unusable.
/// @details The Linux GLX fallback presents through the ZannaGFX CPU window path because the
///          default drawable rejects the fullscreen resolve used by post-FX. On the affected
///          stack, additional single-attachment FBOs can also report complete while discarding
///          color writes. The scene FBO is proven writable by the main render pass, so this
///          fallback ping-pongs between scene color attachment 0 and a dedicated same-precision
///          attachment. Motion vectors remain immutable inputs for every ordered pass, including
///          motion blur, and HDR scenes never pass through an RGBA8 intermediate. The final color
///          is copied back to attachment 0 so final-overlay replay, readback, and presentation all
///          consume the normal scene color target.
/// @param ctx Borrowed initialized OpenGL context containing the writable scene FBO.
/// @param width Positive scene width in pixels.
/// @param height Positive scene height in pixels.
/// @param chain Borrowed usable effect chain whose snapshots are applied in order.
/// @return Non-zero when every enabled effect pass resolves and the final color is in scene
///         color attachment 0.
static int gl_apply_postfx_chain_in_scene(gl_context_t *ctx,
                                          int32_t width,
                                          int32_t height,
                                          const vgfx3d_postfx_chain_t *chain) {
    GLuint source_tex;
    int needs_bloom = 0;

    if (!ctx || !ctx->scene_fbo || !ctx->scene_color_tex || !ctx->scene_motion_tex ||
        !ctx->scene_postfx_tex || !ctx->postfx_program || !ctx->fullscreen_vao || width <= 0 ||
        height <= 0 || !vgfx3d_postfx_chain_is_usable(chain)) {
        return 0;
    }

    /* This fallback must not mutate scene color before discovering that a later pass needs
     * resources it cannot represent inside the scene FBO. Preflight every deterministic
     * prerequisite; runtime GL errors still abort the pass that observes them. */
    for (int32_t i = 0; i < chain->effect_count; i++) {
        vgfx3d_postfx_snapshot_t safe_snapshot;
        vgfx3d_sanitize_postfx_snapshot(&chain->effects[i].snapshot, &safe_snapshot);
        if ((chain->effects[i].type == (int32_t)VGFX3D_POSTFX_EFFECT_TAA &&
             safe_snapshot.taa_enabled) ||
            (chain->effects[i].type == (int32_t)VGFX3D_POSTFX_EFFECT_SSR &&
             safe_snapshot.ssr_enabled)) {
            return 0;
        }
        needs_bloom |= safe_snapshot.bloom_enabled ? 1 : 0;
    }
    if (needs_bloom && ensure_bloom_targets(ctx, width, height) != 0)
        return 0;

    source_tex = ctx->scene_color_tex;
    for (int32_t i = 0; i < chain->effect_count; i++) {
        vgfx3d_postfx_snapshot_t safe_snapshot;
        const int source_is_scene_color = (source_tex == ctx->scene_color_tex) ? 1 : 0;
        const GLenum dst_attachment =
            source_is_scene_color ? GL_COLOR_ATTACHMENT2 : GL_COLOR_ATTACHMENT0;
        const GLuint dst_tex = source_is_scene_color ? ctx->scene_postfx_tex : ctx->scene_color_tex;

        vgfx3d_sanitize_postfx_snapshot(&chain->effects[i].snapshot, &safe_snapshot);
        ctx->postfx_current_bloom_tex = 0;
        if (safe_snapshot.bloom_enabled) {
            ctx->postfx_current_bloom_tex = gl_encode_bloom_chain(
                ctx, source_tex, width, height, safe_snapshot.bloom_threshold);
            if (!ctx->postfx_current_bloom_tex)
                return 0;
        }
        if (!gl_draw_texture_to_target(ctx,
                                       source_tex,
                                       ctx->scene_fbo,
                                       dst_attachment,
                                       width,
                                       height,
                                       width,
                                       height,
                                       &safe_snapshot)) {
            ctx->postfx_current_bloom_tex = 0;
            return 0;
        }
        source_tex = dst_tex;
    }
    ctx->postfx_current_bloom_tex = 0;

    if (source_tex != ctx->scene_color_tex) {
        if (!gl_draw_texture_to_target(ctx,
                                       source_tex,
                                       ctx->scene_fbo,
                                       GL_COLOR_ATTACHMENT0,
                                       width,
                                       height,
                                       width,
                                       height,
                                       NULL)) {
            return 0;
        }
    }
    return 1;
}

/// @brief Apply a full chain of post-FX effects using ping-pong FBO rendering.
/// @details Iterates `chain->effects[0..effect_count-1]`, drawing each pass via
///   `gl_draw_texture_to_target`. Passes alternate between `scene_fbo`/`scene_color_tex`
///   and `postfx_readback_fbo`/`postfx_readback_tex` as source and destination so
///   no pass samples and writes the same texture simultaneously (ping-pong pattern).
///   For the final pass, unless @p force_offscreen_final is non-zero, the output
///   goes directly into @p final_framebuffer / @p final_draw_buffer (usually the
///   default swapchain FBO) at the caller's resolution, avoiding an extra blit.
///
///   When @p force_offscreen_final is set (screenshot readback path), the last
///   pass also writes to an offscreen FBO, and the framebuffer + read-buffer
///   identifiers are returned via @p out_result_framebuffer / @p out_result_read_buffer
///   so the caller can `glReadPixels` from the result without touching the swapchain.
///
///   Returns 0 early if any required inputs are absent, if the postfx readback target
///   can't be allocated, or if any pass encounters a GL error.
/// @param ctx                    Active OpenGL backend context.
/// @param source_tex             GL texture name for the scene colour to process.
/// @param width                  Scene render width in pixels.
/// @param height                 Scene render height in pixels.
/// @param chain                  Effect chain descriptor (enabled flag + effects array).
/// @param final_framebuffer      FBO to write the last pass into (0 = default framebuffer).
/// @param final_draw_buffer      Draw buffer attachment for @p final_framebuffer.
/// @param final_width            Logical destination width for the final pass.
/// @param final_height           Logical destination height for the final pass.
/// @param force_offscreen_final  If non-zero, always write the last pass to an offscreen FBO.
/// @param out_result_framebuffer Receives the FBO holding the last pass's output when
///                               @p force_offscreen_final is set.
/// @param out_result_read_buffer Receives the read-buffer enum for @p out_result_framebuffer.
/// @return 1 on success, 0 if resources are missing or allocation fails.
static int gl_apply_postfx_chain(gl_context_t *ctx,
                                 GLuint source_tex,
                                 int32_t width,
                                 int32_t height,
                                 const vgfx3d_postfx_chain_t *chain,
                                 GLuint final_framebuffer,
                                 GLenum final_draw_buffer,
                                 int32_t final_width,
                                 int32_t final_height,
                                 int force_offscreen_final,
                                 GLuint *out_result_framebuffer,
                                 GLenum *out_result_read_buffer) {
    GLuint last_framebuffer = final_framebuffer;
    GLenum last_read_buffer = final_draw_buffer;

    if (out_result_framebuffer)
        *out_result_framebuffer = 0;
    if (out_result_read_buffer)
        *out_result_read_buffer = ctx ? ctx->default_draw_buffer : GL_BACK;
    if (!ctx || !source_tex || width <= 0 || height <= 0 || final_width <= 0 || final_height <= 0 ||
        !vgfx3d_postfx_chain_is_usable(chain)) {
        return 0;
    }
    if (ensure_postfx_readback_target(ctx, width, height) != 0)
        return 0;
    if (ensure_postfx_scratch_target(ctx, width, height) != 0)
        return 0;

    ctx->postfx_current_bloom_tex = 0;

    for (int32_t i = 0; i < chain->effect_count; i++) {
        vgfx3d_postfx_snapshot_t safe_snapshot;
        const int is_last = (i == chain->effect_count - 1) ? 1 : 0;
        GLuint dst_framebuffer;
        GLenum dst_draw_buffer;
        GLuint dst_texture;
        vgfx3d_sanitize_postfx_snapshot(&chain->effects[i].snapshot, &safe_snapshot);

        /* Plan 05: a TAA entry resolves into its own history target, which then feeds
         * the remaining chain as the pass source. The final-entry case still needs a
         * regular pass below so the result lands in the caller's framebuffer. */
        if (chain->effects[i].type == (int32_t)VGFX3D_POSTFX_EFFECT_TAA &&
            safe_snapshot.taa_enabled) {
            GLuint resolved = gl_encode_taa_pass(ctx, source_tex, width, height, &safe_snapshot);
            if (!resolved) {
                ctx->postfx_current_bloom_tex = 0;
                return 0;
            }
            source_tex = resolved;
        }
        /* Plan 10: an SSR entry composites reflections into its own target, which
         * then feeds the remaining chain exactly like the TAA pre-resolve above. */
        if (chain->effects[i].type == (int32_t)VGFX3D_POSTFX_EFFECT_SSR &&
            safe_snapshot.ssr_enabled) {
            GLuint reflected = gl_encode_ssr_pass(ctx, source_tex, width, height, &safe_snapshot);
            if (!reflected) {
                ctx->postfx_current_bloom_tex = 0;
                return 0;
            }
            source_tex = reflected;
        }
        ctx->postfx_current_bloom_tex = 0;
        if (safe_snapshot.bloom_enabled &&
            chain->effects[i].type != (int32_t)VGFX3D_POSTFX_EFFECT_TAA &&
            chain->effects[i].type != (int32_t)VGFX3D_POSTFX_EFFECT_SSR) {
            ctx->postfx_current_bloom_tex = gl_encode_bloom_chain(
                ctx, source_tex, width, height, safe_snapshot.bloom_threshold);
            if (!ctx->postfx_current_bloom_tex)
                return 0;
        }

        if (is_last && !force_offscreen_final) {
            dst_framebuffer = final_framebuffer;
            dst_draw_buffer = final_draw_buffer;
            dst_texture = 0;
        } else if (source_tex == ctx->scene_color_tex) {
            dst_framebuffer = ctx->postfx_readback_fbo;
            dst_draw_buffer = GL_COLOR_ATTACHMENT0;
            dst_texture = ctx->postfx_readback_tex;
        } else if (source_tex == ctx->postfx_readback_tex) {
            dst_framebuffer = ctx->postfx_scratch_fbo;
            dst_draw_buffer = GL_COLOR_ATTACHMENT0;
            dst_texture = ctx->postfx_scratch_tex;
        } else {
            dst_framebuffer = ctx->postfx_readback_fbo;
            dst_draw_buffer = GL_COLOR_ATTACHMENT0;
            dst_texture = ctx->postfx_readback_tex;
        }

        {
            /* A TAA entry's temporal work already ran; its ping-pong pass is a plain
             * copy (NULL snapshot) so the shared shader's effects stay disabled. */
            const vgfx3d_postfx_snapshot_t *pass_snapshot =
                (chain->effects[i].type == (int32_t)VGFX3D_POSTFX_EFFECT_TAA ||
                 chain->effects[i].type == (int32_t)VGFX3D_POSTFX_EFFECT_SSR)
                    ? NULL
                    : &safe_snapshot;
            const int32_t target_width = (is_last && !force_offscreen_final) ? final_width : width;
            const int32_t target_height =
                (is_last && !force_offscreen_final) ? final_height : height;
            if (!gl_draw_texture_to_target(ctx,
                                           source_tex,
                                           dst_framebuffer,
                                           dst_draw_buffer,
                                           width,
                                           height,
                                           target_width,
                                           target_height,
                                           pass_snapshot)) {
                ctx->postfx_current_bloom_tex = 0;
                return 0;
            }
        }

        last_framebuffer = dst_framebuffer;
        last_read_buffer = dst_draw_buffer;
        if ((!is_last || force_offscreen_final) && dst_texture)
            source_tex = dst_texture;
    }
    ctx->postfx_current_bloom_tex = 0;

    if (force_offscreen_final && last_framebuffer != ctx->postfx_readback_fbo) {
        if (!gl_draw_texture_to_target(ctx,
                                       source_tex,
                                       ctx->postfx_readback_fbo,
                                       GL_COLOR_ATTACHMENT0,
                                       width,
                                       height,
                                       width,
                                       height,
                                       NULL)) {
            ctx->postfx_current_bloom_tex = 0;
            return 0;
        }
        last_framebuffer = ctx->postfx_readback_fbo;
        last_read_buffer = GL_COLOR_ATTACHMENT0;
    }

    if (out_result_framebuffer)
        *out_result_framebuffer = last_framebuffer;
    if (out_result_read_buffer)
        *out_result_read_buffer = last_read_buffer;
    return 1;
}

/// @brief Multiply two size_t values with overflow checking; writes the product to @p out
///   and returns 1, or zeroes @p out and returns 0 on overflow or a NULL out pointer.
/// @param a First unsigned factor.
/// @param b Second unsigned factor.
/// @param out Receives the product on success or zero on failure; may be null.
/// @return 1 when multiplication succeeds without overflow, otherwise 0.
static int gl_checked_mul_size(size_t a, size_t b, size_t *out) {
    if (out)
        *out = 0;
    if (!out)
        return 0;
    if (a != 0 && b > SIZE_MAX / a)
        return 0;
    *out = a * b;
    return 1;
}

/// @brief Read back the RTT color buffer into the user's host RGBA buffer.
///
/// Allocates a transient buffer matching the RT's stride×height, reads
/// pixels from `rtt_fbo` color attachment 0, flips the rows (GL's
/// origin is bottom-left vs. ours top-left), and copies into the
/// user-visible buffer. Marks `color_dirty = 0` so the next draw
/// doesn't trigger a redundant readback. No-op if the receiver isn't
/// the active RTT.
/// @param ctx_ptr Opaque borrowed pointer to the owning @c gl_context_t.
/// @param rt Borrowed runtime render target whose CPU color mirrors receive the readback.
/// @return 1 after a successful readback and mirror update, otherwise 0 with dirty state
///         preserved.
static int gl_sync_render_target_color(void *ctx_ptr, vgfx3d_rendertarget_t *rt) {
    gl_context_t *ctx = (gl_context_t *)ctx_ptr;
    uint8_t *tmp;
    float *tmpf;
    size_t destination_bytes;
    size_t float_count;
    size_t pixel_count;
    size_t float_bytes;
    size_t tight_bytes;
    gl_framebuffer_state_t saved_state;

    if (!ctx || !rt || rt->width <= 0 || rt->width > INT32_MAX / 16 || rt->height <= 0 ||
        !ctx->rtt_fbo || ctx->rtt_target != rt) {
        return 0;
    }
    if (!vgfx3d_rendertarget_ensure_color(rt)) {
        return 0;
    }
    if (!gl_checked_mul_size((size_t)rt->width, (size_t)rt->height, &pixel_count) ||
        !gl_checked_mul_size((size_t)rt->stride, (size_t)rt->height, &destination_bytes) ||
        !gl_checked_mul_size(pixel_count, 4u, &tight_bytes) || destination_bytes < tight_bytes) {
        return 0;
    }

    if (!gl_make_current(ctx))
        return 0;
    gl_capture_framebuffer_state(&saved_state);
    gl.PixelStorei(GL_PACK_ALIGNMENT, 1);
    gl.BindFramebuffer(GL_FRAMEBUFFER, ctx->rtt_fbo);
    gl.ReadBuffer(GL_COLOR_ATTACHMENT0);
    if (vgfx3d_rendertarget_is_hdr(rt)) {
        if (!gl_checked_mul_size(pixel_count, 4u, &float_count) ||
            !gl_checked_mul_size(float_count, sizeof(float), &float_bytes)) {
            gl_restore_framebuffer_state(&saved_state);
            return 0;
        }
        if (!vgfx3d_rendertarget_ensure_hdr_color(rt)) {
            gl_restore_framebuffer_state(&saved_state);
            return 0;
        }
        tmpf = (float *)gl_ensure_readback_scratch(ctx, float_bytes);
        if (!tmpf) {
            gl_restore_framebuffer_state(&saved_state);
            return 0;
        }
        gl.ReadPixels(0, 0, rt->width, rt->height, GL_RGBA, GL_FLOAT, tmpf);
        if (!gl_upload_ok()) {
            gl_restore_framebuffer_state(&saved_state);
            return 0;
        }
        for (int32_t y = 0; y < rt->height; y++) {
            const float *src_row = tmpf + (size_t)(rt->height - 1 - y) * (size_t)rt->width * 4u;
            float *dst_row = rt->hdr_color_buf + (size_t)y * (size_t)rt->width * 4u;
            memcpy(dst_row, src_row, (size_t)rt->width * 4u * sizeof(float));
        }
        /* Convert from the already top-left-oriented HDR mirror so both CPU mirrors match. */
        vgfx3d_copy_linear_rgba32f_to_rgba8(
            rt->color_buf, rt->stride, rt->width, rt->height, rt->hdr_color_buf, rt->width * 16);
        rt->hdr_color_valid = 1;
    } else {
        tmp = gl_ensure_readback_scratch(ctx, tight_bytes);
        if (!tmp) {
            gl_restore_framebuffer_state(&saved_state);
            return 0;
        }
        gl.ReadPixels(0, 0, rt->width, rt->height, GL_RGBA, GL_UNSIGNED_BYTE, tmp);
        if (!gl_upload_ok()) {
            gl_restore_framebuffer_state(&saved_state);
            return 0;
        }
        vgfx3d_flip_rgba_rows(tmp, rt->width, rt->height);
        for (int32_t y = 0; y < rt->height; y++) {
            memcpy(rt->color_buf + (size_t)y * (size_t)rt->stride,
                   tmp + (size_t)y * (size_t)rt->width * 4u,
                   (size_t)rt->width * 4u);
        }
        rt->hdr_color_valid = 0;
    }

    ctx->rtt_color_dirty = 0;
    rt->color_dirty = 0;
    gl_restore_framebuffer_state(&saved_state);
    gl_restore_main_draw_state(ctx);
    return 1;
}
